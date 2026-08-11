#include "oee/ac/solver.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace oee::ac {
namespace {

// Baseline parameters, deliberately in "human" units: dollars, shares,
// minutes. sigma ~ $0.05/share/sqrt(min); eta such that trading 1% of ADV per
// minute costs a few cents. The solver only cares about consistency.
// gamma is kept small enough that eta~ = eta - gamma*tau/2 stays positive
// even at the coarsest grids tested (N = 4 => tau ~ 100 min).
Params baseline() {
  return Params{
      .sigma = 0.05, .gamma = 2.5e-8, .eta = 2.5e-6, .epsilon = 0.01,
      .lambda = 1e-6};
}

constexpr double kX = 1e6;   // 1M shares
constexpr double kT = 390.0; // full trading day in minutes

// Max |closed_form - qp| relative to X.
double max_rel_diff(const std::vector<double>& a, const std::vector<double>& b,
                    double scale) {
  double m = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    m = std::max(m, std::abs(a[i] - b[i]) / scale);
  }
  return m;
}

TEST(ACSolver, ClosedFormMatchesQPReference) {
  // The sinh closed form and the Thomas-algorithm QP solve share no code;
  // agreement to ~1e-12 relative is strong evidence both are right.
  for (const double lambda : {1e-8, 1e-7, 1e-6, 1e-5}) {
    for (const int N : {4, 10, 78, 390}) {
      Params p = baseline();
      p.lambda = lambda;
      const Solution sol = solve(kX, kT, N, p);
      const std::vector<double> qp = solve_qp_reference(kX, kT, N, p);
      ASSERT_EQ(sol.holdings.size(), qp.size());
      EXPECT_LT(max_rel_diff(sol.holdings, qp, kX), 1e-11)
          << "lambda=" << lambda << " N=" << N;
    }
  }
}

TEST(ACSolver, ConditioningDegradesGracefullyAtExtremeKappaT) {
  // Very aggressive risk aversion: kappa*T >> 1. The closed form must not
  // overflow (naive sinh would), and should still track the QP. We accept a
  // looser tolerance here and document where conditioning degrades instead
  // of pretending 1e-11 holds universally.
  Params p = baseline();
  p.lambda = 1e-2;  // kappa*T ~ 1000; naive sinh(kappa*T) overflows at ~710
  const Solution sol = solve(kX, kT, 390, p);
  EXPECT_TRUE(std::isfinite(sol.holdings[1]));
  const std::vector<double> qp = solve_qp_reference(kX, kT, 390, p);
  EXPECT_LT(max_rel_diff(sol.holdings, qp, kX), 1e-9);
}

TEST(ACSolver, LambdaZeroIsExactlyTwap) {
  Params p = baseline();
  p.lambda = 0.0;
  const Solution sol = solve(kX, kT, 10, p);
  EXPECT_EQ(sol.kappa, 0.0);
  for (int j = 0; j <= 10; ++j) {
    EXPECT_DOUBLE_EQ(sol.holdings[static_cast<std::size_t>(j)],
                     kX * (10 - j) / 10.0);
  }
  // All slices identical.
  for (const double n : sol.trades) EXPECT_DOUBLE_EQ(n, kX / 10.0);
}

TEST(ACSolver, TinyLambdaConvergesToTwapSmoothly) {
  // The expm1 ratio form must degrade to the straight line without
  // cancellation blowup — this is where a naive sinh ratio returns NaN.
  Params p = baseline();
  p.lambda = 1e-18;
  const Solution sol = solve(kX, kT, 100, p);
  for (int j = 0; j <= 100; ++j) {
    EXPECT_NEAR(sol.holdings[static_cast<std::size_t>(j)],
                kX * (100 - j) / 100.0, 1e-4 * kX);
    EXPECT_TRUE(std::isfinite(sol.holdings[static_cast<std::size_t>(j)]));
  }
}

TEST(ACSolver, HigherLambdaFrontLoadsExecution) {
  // More risk aversion => sell faster early. Compare holdings midway.
  Params lo = baseline(), hi = baseline();
  lo.lambda = 1e-7;
  hi.lambda = 1e-5;
  const Solution slow = solve(kX, kT, 100, lo);
  const Solution fast = solve(kX, kT, 100, hi);
  EXPECT_LT(fast.holdings[50], slow.holdings[50]);
  // And the risk/cost tradeoff goes the right way: faster = higher expected
  // impact cost, lower variance.
  EXPECT_GT(fast.expected_cost, slow.expected_cost);
  EXPECT_LT(fast.variance, slow.variance);
}

TEST(ACSolver, TrajectoryIsMonotoneAndPinned) {
  const Solution sol = solve(kX, kT, 78, baseline());
  EXPECT_DOUBLE_EQ(sol.holdings.front(), kX);
  EXPECT_DOUBLE_EQ(sol.holdings.back(), 0.0);
  for (std::size_t j = 1; j < sol.holdings.size(); ++j) {
    EXPECT_LE(sol.holdings[j], sol.holdings[j - 1]);
  }
  // Trades sum to X.
  double sum = 0.0;
  for (const double n : sol.trades) sum += n;
  EXPECT_NEAR(sum, kX, 1e-6 * kX);
}

TEST(ACSolver, DiscreteKappaDiffersFromContinuousByOrderTauSquared) {
  // kappa_discrete = (2/tau) asinh(ktilde*tau/2)
  //                = ktilde * (1 - (ktilde*tau)^2/24 + O(tau^4)).
  // Check the leading-order gap at two grid resolutions: quartering tau must
  // shrink the gap ~16x. gamma = 0 here so eta~ (and hence ktilde) is the
  // same at both resolutions — otherwise the comparison mixes two different
  // continuous limits.
  Params p = baseline();
  p.gamma = 0.0;
  p.lambda = 1e-6;
  const Solution coarse = solve(kX, kT, 39, p);
  const Solution fine = solve(kX, kT, 156, p);
  const Solution cont = solve(kX, kT, 39, p, /*continuous_kappa=*/true);
  const double ktilde = cont.kappa;

  const double gap_coarse = ktilde - coarse.kappa;
  const double gap_fine = ktilde - fine.kappa;
  EXPECT_GT(gap_coarse, 0.0);  // discrete kappa is always smaller
  EXPECT_NEAR(gap_coarse / gap_fine, 16.0, 0.5);
}

TEST(ACSolver, CostFunctionalsHandCheckedOnTinyCase) {
  // N=2, X=100, T=2 (tau=1): x = {100, x1, 0}, trades {100-x1, x1}.
  // Verify E and V against direct arithmetic for a NON-optimal trajectory.
  Params p{.sigma = 0.5, .gamma = 1e-4, .eta = 1e-3, .epsilon = 0.01,
           .lambda = 0.0};
  const std::vector<double> x = {100.0, 30.0, 0.0};
  const double tau = 1.0;
  const double eta_tilde = p.eta - 0.5 * p.gamma * tau;  // 9.5e-4
  const double e_expected = 0.5 * p.gamma * 100.0 * 100.0    // permanent
                            + p.epsilon * 100.0               // spread
                            + eta_tilde / tau * (70.0 * 70.0 + 30.0 * 30.0);
  const double v_expected = p.sigma * p.sigma * tau * 30.0 * 30.0;
  EXPECT_DOUBLE_EQ(expected_cost(x, 2.0, p), e_expected);
  EXPECT_DOUBLE_EQ(variance(x, 2.0, p), v_expected);
}

TEST(ACSolver, RejectsIllPosedInputs) {
  Params p = baseline();
  EXPECT_THROW(solve(kX, 0.0, 10, p), std::invalid_argument);
  EXPECT_THROW(solve(kX, kT, 0, p), std::invalid_argument);

  Params neg = baseline();
  neg.lambda = -1.0;
  EXPECT_THROW(solve(kX, kT, 10, neg), std::invalid_argument);

  // eta~ <= 0: permanent impact dominates at this tau — must throw with the
  // well-posedness message, not return a garbage trajectory.
  Params ill = baseline();
  ill.gamma = 1.0;  // gamma*tau/2 >> eta
  EXPECT_THROW(solve(kX, kT, 10, ill), std::invalid_argument);
}

TEST(ACSolver, OptimalitySpotCheck) {
  // The solver's trajectory should beat nearby perturbations on E + lambda*V.
  Params p = baseline();
  const Solution sol = solve(kX, kT, 20, p);
  const double u_opt =
      sol.expected_cost + p.lambda * sol.variance;
  for (const std::size_t j : {5u, 10u, 15u}) {
    for (const double bump : {-0.01 * kX, 0.01 * kX}) {
      std::vector<double> x = sol.holdings;
      x[j] += bump;
      const double u =
          expected_cost(x, kT, p) + p.lambda * variance(x, kT, p);
      EXPECT_GT(u, u_opt) << "perturbation at j=" << j << " improved U";
    }
  }
}

}  // namespace
}  // namespace oee::ac
