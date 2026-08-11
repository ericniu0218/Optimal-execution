#pragma once

#include <vector>

namespace oee::ac {

// ---------------------------------------------------------------------------
// Exact discrete-time Almgren-Chriss optimal liquidation.
//
// Setup (Almgren & Chriss, "Optimal Execution of Portfolio Transactions",
// J. Risk 2000): liquidate X shares over [0, T] in N intervals of length
// tau = T/N. Holdings x_0 = X >= x_1 >= ... >= x_N = 0; trade in interval k
// is n_k = x_{k-1} - x_k.
//
//   price dynamics : S_k = S_{k-1} + sigma*sqrt(tau)*xi_k - gamma*n_k
//   execution price: S~_k = S_{k-1} - (epsilon*sgn(n_k) + (eta/tau)*n_k)
//
// Implementation shortfall IS = X*S_0 - sum_k n_k*S~_k has
//
//   E[IS] = (gamma/2)*X^2 + epsilon*sum|n_k| + (eta~/tau)*sum n_k^2
//   V[IS] = sigma^2*tau*sum_{k=1}^{N-1} x_k^2
//   eta~  = eta - gamma*tau/2      (discreteness correction)
//
// Minimizing E + lambda*V over {x_j}: the first-order condition is the linear
// second-order difference equation
//
//   (x_{j-1} - 2x_j + x_{j+1}) / tau^2 = ktilde^2 * x_j,
//   ktilde^2 = lambda*sigma^2 / eta~,
//
// whose decay rate kappa solves 2(cosh(kappa*tau) - 1)/tau^2 = ktilde^2, i.e.
//
//   kappa = (2/tau) * asinh(ktilde*tau/2)          [exact discrete]
//
// (continuous limit: kappa -> ktilde as tau -> 0). Solution with boundary
// conditions x_0 = X, x_N = 0:
//
//   x_j = X * sinh(kappa*(T - t_j)) / sinh(kappa*T)
//
// NOTE the eta~ inside ktilde: using raw eta silently mixes the discrete
// recursion with its continuous limit and breaks any tight numerical
// comparison against a direct QP solve of the same objective.
//
// Numerical form: sinh ratios overflow for large kappa*T and cancel
// catastrophically for small kappa*T. We evaluate
//
//   sinh(a)/sinh(b) = e^{a-b} * expm1(-2a) / expm1(-2b),   0 < a <= b
//
// which is stable at both extremes; lambda = 0 (kappa = 0) is the exact
// straight-line TWAP and is returned as such.
//
// Units: the solver is unit-agnostic. sigma, gamma, eta, epsilon, lambda must
// simply be mutually consistent (e.g. dollars/share/sqrt(sec), etc.). The
// calibration layer owns unit conversion.
// ---------------------------------------------------------------------------

struct Params {
  double sigma;    // price volatility per sqrt(unit time)
  double gamma;    // permanent impact: price shift per share traded
  double eta;      // temporary impact: price penalty per (share / unit time)
  double epsilon;  // fixed cost per share (half-spread + fees); affects E only
  double lambda;   // risk aversion; lambda = 0 recovers TWAP
  // Expected price drift per unit time, signed FOR A SELL PROGRAM: alpha > 0
  // means the price is expected to rise, so holding inventory pays and the
  // optimal schedule slows down. A buy program passes -alpha.
  double alpha = 0.0;
};

struct Solution {
  std::vector<double> holdings;  // x_0..x_N (size N+1), x_0 = X, x_N = 0
  std::vector<double> trades;    // n_1..n_N (size N), n_k = x_{k-1} - x_k
  double kappa;                  // trajectory decay rate (1/unit time)
  double expected_cost;          // E[IS]
  double variance;               // Var[IS]
};

// With drift the cost gains a term -alpha*tau*sum_{k=1}^{N-1} x_k (holding
// x_k through period k+1 earns alpha*tau per share), so the first-order
// condition becomes the SAME linear recursion with a constant forcing term:
//
//   (x_{j-1} - 2x_j + x_{j+1}) / tau^2 = ktilde^2 * x_j - alpha/(2*eta~)
//
// whose particular solution is the constant xbar = alpha / (2*lambda*sigma^2)
// — the inventory the drift alone would have you hold. The general solution
// is that constant plus the same exponentials as before:
//
//   x_j = xbar + u*exp(-kappa*t_j)
//              - (xbar + u*exp(-kappa*T)) * sinh(kappa*t_j)/sinh(kappa*T),
//   u = X - xbar
//
// (alpha = 0 collapses this to the plain sinh solution.) The lambda = 0 case
// is separate and quadratic: x_j = X(1 - t/T) + alpha/(4*eta~) * t(T - t).
//
// Solve for the optimal trajectory. Throws std::invalid_argument on
// ill-posed inputs, in particular eta~ = eta - gamma*tau/2 <= 0 (the AC
// well-posedness condition: temporary impact must dominate at this tau).
// `continuous_kappa` selects kappa = ktilde (continuous-time approximation)
// instead of the exact discrete rate — exposed so "how much does the
// discreteness correction matter" is a config flag, not a code edit.
Solution solve(double X, double T, int N, const Params& p,
               bool continuous_kappa = false);

// Independent numerical reference for testing: solves the identical QP
// (tridiagonal, strictly convex) with the Thomas algorithm. Shares no code
// path with the closed form. Returns x_0..x_N.
std::vector<double> solve_qp_reference(double X, double T, int N,
                                       const Params& p);

// Cost functionals for an arbitrary (not necessarily optimal) trajectory
// x_0..x_N — used for benchmarking TWAP/VWAP/POV schedules on the same axes.
double expected_cost(const std::vector<double>& holdings, double T,
                     const Params& p);
double variance(const std::vector<double>& holdings, double T, const Params& p);

}  // namespace oee::ac
