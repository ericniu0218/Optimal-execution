#include "oee/ac/solver.hpp"

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace oee::ac {
namespace {

void validate(double X, double T, int N, const Params& p, double eta_tilde) {
  if (!(T > 0.0)) throw std::invalid_argument("ac::solve: T must be > 0");
  if (N < 1) throw std::invalid_argument("ac::solve: N must be >= 1");
  if (!(p.eta > 0.0)) throw std::invalid_argument("ac::solve: eta must be > 0");
  if (p.lambda < 0.0) throw std::invalid_argument("ac::solve: lambda must be >= 0");
  if (p.sigma < 0.0) throw std::invalid_argument("ac::solve: sigma must be >= 0");
  if (!(eta_tilde > 0.0)) {
    throw std::invalid_argument(
        "ac::solve: eta~ = eta - gamma*tau/2 = " + std::to_string(eta_tilde) +
        " <= 0; the discrete problem is ill-posed at this tau (permanent "
        "impact dominates temporary). Decrease tau or revisit parameters.");
  }
  (void)X;  // any real X is fine; buy programs are the sign flip
}

// Stable sinh(a)/sinh(b) for 0 <= a <= b:
//   sinh(a)/sinh(b) = e^{a-b} * (1 - e^{-2a}) / (1 - e^{-2b})
//                   = e^{a-b} * expm1(-2a) / expm1(-2b).
// No overflow (all exponents <= 0) and no cancellation for small arguments,
// since expm1 is exact near 0. Requires b > 0.
double sinh_ratio(double a, double b) {
  return std::exp(a - b) * std::expm1(-2.0 * a) / std::expm1(-2.0 * b);
}

}  // namespace

Solution solve(double X, double T, int N, const Params& p,
               bool continuous_kappa) {
  const double tau = T / N;
  const double eta_tilde = p.eta - 0.5 * p.gamma * tau;
  validate(X, T, N, p, eta_tilde);

  Solution sol;
  sol.holdings.resize(static_cast<std::size_t>(N) + 1);
  sol.trades.resize(static_cast<std::size_t>(N));

  // ktilde^2 = lambda*sigma^2/eta~ ; kappa via the exact discrete relation
  // 2(cosh(kappa*tau)-1)/tau^2 = ktilde^2  =>  kappa = (2/tau)asinh(ktilde*tau/2)
  // (using cosh(u)-1 = 2 sinh^2(u/2)).
  const double ktilde = std::sqrt(p.lambda * p.sigma * p.sigma / eta_tilde);
  const double kappa =
      continuous_kappa ? ktilde : (2.0 / tau) * std::asinh(0.5 * ktilde * tau);
  sol.kappa = kappa;

  if (kappa == 0.0) {
    // Risk-neutral limit (lambda = 0 or sigma = 0). Without drift this is
    // exactly TWAP; with drift the recursion is x'' = -alpha/(2 eta~),
    // whose solution is the straight line plus a parabola vanishing at
    // both endpoints.
    const double bow = p.alpha / (4.0 * eta_tilde);
    for (int j = 0; j <= N; ++j) {
      const double t_j = tau * j;
      sol.holdings[static_cast<std::size_t>(j)] =
          X * static_cast<double>(N - j) / static_cast<double>(N) +
          bow * t_j * (T - t_j);
    }
  } else {
    // xbar is the constant particular solution: the inventory the drift
    // alone would justify holding. u is the deviation from it at t=0.
    const double xbar = p.alpha / (2.0 * p.lambda * p.sigma * p.sigma);
    const double u = X - xbar;
    const double e_negkT = std::exp(-kappa * T);
    for (int j = 0; j <= N; ++j) {
      const double t_j = tau * j;
      sol.holdings[static_cast<std::size_t>(j)] =
          xbar + u * std::exp(-kappa * t_j) -
          (xbar + u * e_negkT) * sinh_ratio(kappa * t_j, kappa * T);
    }
  }
  sol.holdings[0] = X;    // pin boundaries exactly against rounding
  sol.holdings[static_cast<std::size_t>(N)] = 0.0;

  for (int k = 1; k <= N; ++k) {
    sol.trades[static_cast<std::size_t>(k - 1)] =
        sol.holdings[static_cast<std::size_t>(k - 1)] -
        sol.holdings[static_cast<std::size_t>(k)];
  }

  sol.expected_cost = expected_cost(sol.holdings, T, p);
  sol.variance      = variance(sol.holdings, T, p);
  return sol;
}

std::vector<double> solve_qp_reference(double X, double T, int N,
                                       const Params& p) {
  const double tau = T / N;
  const double eta_tilde = p.eta - 0.5 * p.gamma * tau;
  validate(X, T, N, p, eta_tilde);

  std::vector<double> x(static_cast<std::size_t>(N) + 1, 0.0);
  x[0] = X;
  x[static_cast<std::size_t>(N)] = 0.0;
  if (N == 1) return x;  // no interior points: single-shot execution

  // Minimize  (eta~/tau) sum (x_{k-1}-x_k)^2 + lambda sigma^2 tau sum x_k^2
  // over interior x_1..x_{N-1}. Stationarity at j:
  //   (2 eta~/tau)(2x_j - x_{j-1} - x_{j+1}) + 2 lambda sigma^2 tau x_j = 0
  // => tridiagonal system  A x = b  with
  //   diag = 4 eta~/tau + 2 lambda sigma^2 tau,  off = -2 eta~/tau,
  //   b_1 = (2 eta~/tau) X + alpha*tau  (x_0 boundary + drift),
  //   b_else = alpha*tau.
  // Strictly convex (eta~ > 0) => unique minimum. Solved with the Thomas
  // algorithm; deliberately shares no code with the sinh closed form.
  const int m = N - 1;
  const double off  = -2.0 * eta_tilde / tau;
  const double diag = 4.0 * eta_tilde / tau +
                      2.0 * p.lambda * p.sigma * p.sigma * tau;

  std::vector<double> c(static_cast<std::size_t>(m), 0.0);  // superdiagonal'
  std::vector<double> d(static_cast<std::size_t>(m), 0.0);  // rhs'

  // Forward sweep.
  const double drift_rhs = p.alpha * tau;
  c[0] = off / diag;
  d[0] = (-off * X + drift_rhs) / diag;  // b_1 = (2 eta~/tau) X + alpha*tau
  for (int j = 1; j < m; ++j) {
    const double denom = diag - off * c[static_cast<std::size_t>(j - 1)];
    c[static_cast<std::size_t>(j)] = off / denom;
    d[static_cast<std::size_t>(j)] =
        (drift_rhs - off * d[static_cast<std::size_t>(j - 1)]) / denom;
  }
  // Back substitution.
  x[static_cast<std::size_t>(m)] = d[static_cast<std::size_t>(m - 1)];
  for (int j = m - 1; j >= 1; --j) {
    x[static_cast<std::size_t>(j)] =
        d[static_cast<std::size_t>(j - 1)] -
        c[static_cast<std::size_t>(j - 1)] * x[static_cast<std::size_t>(j + 1)];
  }
  return x;
}

double expected_cost(const std::vector<double>& holdings, double T,
                     const Params& p) {
  if (holdings.size() < 2) {
    throw std::invalid_argument("ac::expected_cost: need at least x_0 and x_N");
  }
  const int N = static_cast<int>(holdings.size()) - 1;
  const double tau = T / N;
  const double eta_tilde = p.eta - 0.5 * p.gamma * tau;

  const double X = holdings.front() - holdings.back();
  double sum_abs = 0.0, sum_sq = 0.0;
  for (int k = 1; k <= N; ++k) {
    const double n_k = holdings[static_cast<std::size_t>(k - 1)] -
                       holdings[static_cast<std::size_t>(k)];
    sum_abs += std::abs(n_k);
    sum_sq  += n_k * n_k;
  }
  // Drift gain: holding x_k through period k+1 earns alpha*tau per share.
  double sum_hold = 0.0;
  for (int k = 1; k < N; ++k) sum_hold += holdings[static_cast<std::size_t>(k)];

  return 0.5 * p.gamma * X * X + p.epsilon * sum_abs +
         (eta_tilde / tau) * sum_sq - p.alpha * tau * sum_hold;
}

double variance(const std::vector<double>& holdings, double T,
                const Params& p) {
  if (holdings.size() < 2) {
    throw std::invalid_argument("ac::variance: need at least x_0 and x_N");
  }
  const int N = static_cast<int>(holdings.size()) - 1;
  const double tau = T / N;
  // Var[IS] = sigma^2 tau sum_{k=1}^{N-1} x_k^2 : holdings x_k are exposed to
  // the shock in interval k+1; x_0 is not (trades settle against S_0's info)
  // and x_N = 0.
  double sum_sq = 0.0;
  for (int k = 1; k < N; ++k) {
    const double x_k = holdings[static_cast<std::size_t>(k)];
    sum_sq += x_k * x_k;
  }
  return p.sigma * p.sigma * tau * sum_sq;
}

}  // namespace oee::ac
