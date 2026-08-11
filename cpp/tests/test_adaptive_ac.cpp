#include "oee/strategy/adaptive_ac.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "oee/ac/solver.hpp"
#include "oee/strategy/scheduled.hpp"

namespace oee {
namespace {

constexpr Nanos kT0 = 34200 * kNanosPerSec;

ac::Params base_params(double lambda = 1e-6) {
  return ac::Params{.sigma = 0.05, .gamma = 2.5e-8, .eta = 2.5e-6,
                    .epsilon = 0.01, .lambda = lambda};
}

// A book whose mid follows a prescribed path, one row per minute.
struct DriftingBook {
  BookTape tape;
  explicit DriftingBook(const std::vector<PriceTicks>& mids) {
    tape.levels = 1;
    for (const PriceTicks m : mids) {
      tape.ask_price.push_back(m + 50);
      tape.ask_size.push_back(1'000'000);
      tape.bid_price.push_back(m - 50);
      tape.bid_size.push_back(1'000'000);
    }
  }
  ShiftedBookView at(std::size_t row) const {
    return ShiftedBookView(tape, row, 0);
  }
};

ParentOrder parent(Qty qty, int slices) {
  return ParentOrder{.side = Side::kSell, .quantity = qty, .start = kT0,
                     .end = kT0 + static_cast<Nanos>(slices) * 60 * kNanosPerSec,
                     .num_slices = slices};
}

// Run the strategy against a mid path; return the executed child sizes.
std::vector<Qty> run(AdaptiveAcStrategy& s, const ParentOrder& p,
                     const DriftingBook& book) {
  s.on_start(p);
  std::vector<Qty> children;
  Qty remaining = p.quantity;
  for (int k = 0; k < p.num_slices; ++k) {
    const Nanos now = kT0 + static_cast<Nanos>(k) * 60 * kNanosPerSec;
    const ExecutionState st{.slice_idx = k, .remaining = remaining,
                            .market_volume = 0};
    const Qty child = s.on_slice(now, book.at(static_cast<std::size_t>(k)), st);
    EXPECT_GE(child, 0);
    EXPECT_LE(child, remaining);
    children.push_back(child);
    remaining -= child;
  }
  EXPECT_EQ(remaining, 0) << "adaptive schedule must always complete";
  return children;
}

std::vector<PriceTicks> flat(int n, PriceTicks m = 5000000) {
  return std::vector<PriceTicks>(static_cast<std::size_t>(n), m);
}

std::vector<PriceTicks> ramp(int n, PriceTicks m0, PriceTicks per_step) {
  std::vector<PriceTicks> v;
  for (int i = 0; i < n; ++i) v.push_back(m0 + per_step * i);
  return v;
}

// ---------------------------------------------------------------------------
// The drift solver itself
// ---------------------------------------------------------------------------

TEST(AcDrift, MatchesQpReferenceWithDrift) {
  // The acceptance bar from the drift-free solver, re-applied: the closed
  // form with a forcing term must match an independent tridiagonal solve.
  for (const double alpha : {-0.05, -0.01, 0.0, 0.01, 0.05}) {
    for (const double lambda : {1e-7, 1e-6, 1e-5}) {
      ac::Params p = base_params(lambda);
      p.alpha = alpha;
      const ac::Solution sol = ac::solve(1e6, 390.0, 78, p);
      const std::vector<double> qp = ac::solve_qp_reference(1e6, 390.0, 78, p);
      double worst = 0.0;
      for (std::size_t i = 0; i < qp.size(); ++i) {
        worst = std::max(worst, std::abs(sol.holdings[i] - qp[i]) / 1e6);
      }
      EXPECT_LT(worst, 1e-11) << "alpha=" << alpha << " lambda=" << lambda;
    }
  }
}

TEST(AcDrift, ZeroDriftReproducesPlainSolution) {
  ac::Params p = base_params();
  const ac::Solution with_zero = ac::solve(1e6, 390.0, 50, p);
  p.alpha = 0.0;
  const ac::Solution plain = ac::solve(1e6, 390.0, 50, p);
  for (std::size_t j = 0; j < plain.holdings.size(); ++j) {
    EXPECT_DOUBLE_EQ(with_zero.holdings[j], plain.holdings[j]);
  }
}

TEST(AcDrift, PositiveDriftSlowsASellAndNegativeSpeedsItUp) {
  // Selling into a rising market: hold inventory longer. Falling: dump it.
  ac::Params slow = base_params(), fast = base_params(), flat_p = base_params();
  slow.alpha = 0.02;
  fast.alpha = -0.02;
  const auto a = ac::solve(1e6, 390.0, 50, slow).holdings;
  const auto b = ac::solve(1e6, 390.0, 50, flat_p).holdings;
  const auto c = ac::solve(1e6, 390.0, 50, fast).holdings;
  for (std::size_t j = 1; j + 1 < b.size(); ++j) {
    EXPECT_GT(a[j], b[j]) << "j=" << j;  // rising => slower
    EXPECT_LT(c[j], b[j]) << "j=" << j;  // falling => faster
  }
}

TEST(AcDrift, RiskNeutralDriftBowsTheTwapLine) {
  // lambda = 0 branch: the schedule is TWAP plus a parabola, so it is bowed
  // above the straight line but still hits both endpoints exactly.
  ac::Params p = base_params(0.0);
  p.alpha = 0.01;
  const ac::Solution sol = ac::solve(1e6, 390.0, 40, p);
  EXPECT_DOUBLE_EQ(sol.holdings.front(), 1e6);
  EXPECT_DOUBLE_EQ(sol.holdings.back(), 0.0);
  for (int j = 1; j < 40; ++j) {
    const double twap = 1e6 * (40 - j) / 40.0;
    EXPECT_GT(sol.holdings[static_cast<std::size_t>(j)], twap);
  }
}

TEST(AcDrift, DriftGainEntersExpectedCost) {
  // Holding inventory into a rising market reduces expected cost by
  // alpha*tau*sum(x_k) and nothing else changes.
  const std::vector<double> x = {100.0, 60.0, 30.0, 0.0};
  ac::Params p = base_params();
  const double without = ac::expected_cost(x, 3.0, p);
  p.alpha = 0.5;
  const double with = ac::expected_cost(x, 3.0, p);
  EXPECT_NEAR(without - with, 0.5 * 1.0 * (60.0 + 30.0), 1e-12);
}

// ---------------------------------------------------------------------------
// The adaptive strategy
// ---------------------------------------------------------------------------

TEST(AdaptiveAc, FlatMarketTracksStaticAc) {
  // With no drift to detect, the adaptive schedule must reproduce the static
  // AC trajectory — this is the "adaptivity adds nothing without new
  // information" property, made concrete.
  const int n = 30;
  AdaptiveAcStrategy adaptive(base_params(), /*horizon=*/30.0, 0.25, 5);
  const DriftingBook book(flat(n));
  const auto children = run(adaptive, parent(30000, n), book);

  ScheduledStrategy stat("AC", ac_weights(n, 30.0, base_params()));
  const ParentOrder p = parent(30000, n);
  stat.on_start(p);
  Qty remaining = p.quantity;
  for (int k = 0; k < n; ++k) {
    const ExecutionState st{.slice_idx = k, .remaining = remaining,
                            .market_volume = 0};
    const Qty want = stat.on_slice(kT0, book.at(0), st);
    EXPECT_NEAR(static_cast<double>(children[static_cast<std::size_t>(k)]),
                static_cast<double>(want), 2.0)
        << "slice " << k;
    remaining -= children[static_cast<std::size_t>(k)];
  }
  EXPECT_DOUBLE_EQ(adaptive.last_alpha(), 0.0);
}

TEST(AdaptiveAc, RisingMarketDefersExecution) {
  const int n = 30;
  const DriftingBook rising(ramp(n, 5000000, 2000));   // +0.2$/min
  const DriftingBook flat_book(flat(n));

  AdaptiveAcStrategy a(base_params(), 30.0, 0.5, 5);
  AdaptiveAcStrategy b(base_params(), 30.0, 0.5, 5);
  const auto up = run(a, parent(30000, n), rising);
  const auto level = run(b, parent(30000, n), flat_book);

  EXPECT_GT(a.last_alpha(), 0.0);
  // Cumulative execution must lag the flat-market schedule once the drift
  // has been detected (min_obs = 5 slices).
  Qty cum_up = 0, cum_level = 0;
  for (int k = 0; k < 20; ++k) {
    cum_up += up[static_cast<std::size_t>(k)];
    cum_level += level[static_cast<std::size_t>(k)];
  }
  EXPECT_LT(cum_up, cum_level);
}

TEST(AdaptiveAc, FallingMarketAcceleratesExecution) {
  const int n = 30;
  const DriftingBook falling(ramp(n, 5000000, -2000));
  AdaptiveAcStrategy a(base_params(), 30.0, 0.5, 5);
  AdaptiveAcStrategy b(base_params(), 30.0, 0.5, 5);
  const auto down = run(a, parent(30000, n), falling);
  const auto level = run(b, parent(30000, n), DriftingBook(flat(n)));

  EXPECT_LT(a.last_alpha(), 0.0);
  Qty cum_down = 0, cum_level = 0;
  for (int k = 0; k < 20; ++k) {
    cum_down += down[static_cast<std::size_t>(k)];
    cum_level += level[static_cast<std::size_t>(k)];
  }
  EXPECT_GT(cum_down, cum_level);
}

TEST(AdaptiveAc, BuySideFlipsTheDriftSign) {
  // A rising market is bad news for a buyer: hurry, do not wait.
  const int n = 30;
  const DriftingBook rising(ramp(n, 5000000, 2000));
  AdaptiveAcStrategy s(base_params(), 30.0, 0.5, 5);
  ParentOrder p = parent(30000, n);
  p.side = Side::kBuy;
  run(s, p, rising);
  EXPECT_LT(s.last_alpha(), 0.0);
}

TEST(AdaptiveAc, ZeroShrinkIsStaticAc) {
  const int n = 30;
  const DriftingBook rising(ramp(n, 5000000, 5000));
  AdaptiveAcStrategy off(base_params(), 30.0, /*shrink=*/0.0, 5);
  AdaptiveAcStrategy on(base_params(), 30.0, /*shrink=*/0.5, 5);
  const auto ignored = run(off, parent(30000, n), rising);
  const auto acted = run(on, parent(30000, n), rising);
  EXPECT_DOUBLE_EQ(off.last_alpha(), 0.0);
  EXPECT_NE(ignored, acted);
}

TEST(AdaptiveAc, AlwaysCompletesUnderExtremeDrift) {
  // A drift large enough to make the unconstrained optimum want to BUY must
  // still liquidate fully by the deadline (clamped, never reversed).
  const int n = 20;
  const DriftingBook screaming(ramp(n, 5000000, 50000));  // +$5/min
  AdaptiveAcStrategy s(base_params(), 20.0, 1.0, 3);
  const auto children = run(s, parent(20000, n), screaming);  // asserts completion
  for (const Qty c : children) EXPECT_GE(c, 0);
}

TEST(AdaptiveAc, ValidatesConfiguration) {
  EXPECT_THROW(AdaptiveAcStrategy(base_params(), 0.0), std::invalid_argument);
  EXPECT_THROW(AdaptiveAcStrategy(base_params(), 30.0, -0.1),
               std::invalid_argument);
  EXPECT_THROW(AdaptiveAcStrategy(base_params(), 30.0, 1.5),
               std::invalid_argument);
  AdaptiveAcStrategy s(base_params(), 30.0);
  EXPECT_THROW(s.on_start(parent(0, 10)), std::invalid_argument);
}

}  // namespace
}  // namespace oee
