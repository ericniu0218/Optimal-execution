#include "oee/strategy/pov.hpp"
#include "oee/strategy/scheduled.hpp"

#include <gtest/gtest.h>

#include <numeric>
#include <stdexcept>

namespace oee {
namespace {

constexpr Nanos kT0 = 34200 * kNanosPerSec;

// Strategies never dereference the book in these tests (TWAP/VWAP/AC/POV are
// schedule- or volume-driven), but the interface requires one; a minimal
// single-row tape suffices.
struct BookFixture {
  BookTape tape;
  BookFixture() {
    tape.levels = 1;
    tape.ask_price = {5000100};
    tape.ask_size = {100};
    tape.bid_price = {5000000};
    tape.bid_size = {100};
  }
  ShiftedBookView view() const { return ShiftedBookView(tape, 0, 0); }
};

ParentOrder parent(Qty qty, int slices) {
  return ParentOrder{.side = Side::kSell,
                     .quantity = qty,
                     .start = kT0,
                     .end = kT0 + 600 * kNanosPerSec,
                     .num_slices = slices};
}

// Drive a strategy through all slices with a fill-ratio callback deciding
// how much of each child actually executes. Returns total executed.
template <typename FillFn>
Qty run_slices(IExecutionStrategy& strat, const ParentOrder& p,
               FillFn fill_ratio) {
  BookFixture book;
  strat.on_start(p);
  Qty remaining = p.quantity;
  for (int k = 0; k < p.num_slices; ++k) {
    const ExecutionState st{
        .slice_idx = k, .remaining = remaining, .market_volume = 0};
    const Qty child = strat.on_slice(kT0 + k, book.view(), st);
    EXPECT_GE(child, 0);
    EXPECT_LE(child, remaining);
    remaining -= fill_ratio(k, child);
  }
  return p.quantity - remaining;
}

// ---------------------------------------------------------------------------
// Weight curves
// ---------------------------------------------------------------------------

TEST(Schedules, TwapWeightsAreLinear) {
  const auto w = twap_weights(4);
  ASSERT_EQ(w.size(), 5u);
  EXPECT_DOUBLE_EQ(w[0], 1.0);
  EXPECT_DOUBLE_EQ(w[1], 0.75);
  EXPECT_DOUBLE_EQ(w[2], 0.5);
  EXPECT_DOUBLE_EQ(w[4], 0.0);
}

TEST(Schedules, VwapWeightsFormAUCurve) {
  const int n = 10;
  const auto w = vwap_weights(n, 2.0);
  ASSERT_EQ(w.size(), 11u);
  EXPECT_DOUBLE_EQ(w.front(), 1.0);
  EXPECT_DOUBLE_EQ(w.back(), 0.0);
  // Slice fractions f_j = w_j - w_{j+1} must sum to 1, be positive, and be
  // U-shaped: heavier at open/close than midday, symmetric.
  std::vector<double> f(n);
  for (int j = 0; j < n; ++j) {
    f[static_cast<std::size_t>(j)] = w[static_cast<std::size_t>(j)] -
                                     w[static_cast<std::size_t>(j) + 1];
    EXPECT_GT(f[static_cast<std::size_t>(j)], 0.0);
  }
  EXPECT_NEAR(std::accumulate(f.begin(), f.end(), 0.0), 1.0, 1e-12);
  EXPECT_GT(f.front(), f[4]);          // open > midday
  EXPECT_GT(f.back(), f[4]);           // close > midday
  EXPECT_NEAR(f.front(), f.back(), 1e-12);  // symmetric smile
}

TEST(Schedules, VwapWithZeroSmileIsTwap) {
  const auto v = vwap_weights(8, 0.0);
  const auto t = twap_weights(8);
  for (std::size_t j = 0; j < v.size(); ++j) EXPECT_NEAR(v[j], t[j], 1e-12);
}

TEST(Schedules, AcWeightsWithZeroLambdaAreTwap) {
  ac::Params p{.sigma = 0.05, .gamma = 2.5e-8, .eta = 2.5e-6,
               .epsilon = 0.01, .lambda = 0.0};
  const auto ac = ac_weights(10, 390.0, p);
  const auto tw = twap_weights(10);
  for (std::size_t j = 0; j < ac.size(); ++j) EXPECT_NEAR(ac[j], tw[j], 1e-12);
}

TEST(Schedules, AcWeightsFrontLoadWithRiskAversion) {
  ac::Params p{.sigma = 0.05, .gamma = 2.5e-8, .eta = 2.5e-6,
               .epsilon = 0.01, .lambda = 1e-5};
  const auto w = ac_weights(10, 390.0, p);
  const auto tw = twap_weights(10);
  // Risk-averse holdings decay strictly below the TWAP line mid-schedule.
  for (std::size_t j = 1; j + 1 < w.size(); ++j) EXPECT_LT(w[j], tw[j]);
}

// ---------------------------------------------------------------------------
// ScheduledStrategy execution
// ---------------------------------------------------------------------------

TEST(ScheduledStrategy, TwapSendsEqualChildrenUnderFullFills) {
  ScheduledStrategy s("TWAP", twap_weights(4));
  const ParentOrder p = parent(1000, 4);
  BookFixture book;
  s.on_start(p);
  Qty remaining = p.quantity;
  for (int k = 0; k < 4; ++k) {
    const ExecutionState st{.slice_idx = k, .remaining = remaining,
                            .market_volume = 0};
    const Qty child = s.on_slice(kT0 + k, book.view(), st);
    EXPECT_EQ(child, 250) << "slice " << k;
    remaining -= child;
  }
  EXPECT_EQ(remaining, 0);
}

TEST(ScheduledStrategy, PartialFillsRollIntoLaterSlices) {
  ScheduledStrategy s("TWAP", twap_weights(4));
  // Slice 0's child (250) fills only 100; slice 1 must target cumulative
  // 500 and send 400. Full completion by the end regardless.
  const Qty executed = run_slices(s, parent(1000, 4), [](int k, Qty child) {
    return k == 0 ? Qty{100} : child;
  });
  EXPECT_EQ(executed, 1000);
}

TEST(ScheduledStrategy, CatchUpChildIsCappedByRemaining) {
  ScheduledStrategy s("TWAP", twap_weights(2));
  const ParentOrder p = parent(100, 2);
  BookFixture book;
  s.on_start(p);
  // Nothing filled in slice 0: final slice must send exactly remaining, no
  // more, even though it is "behind schedule".
  const ExecutionState st{.slice_idx = 1, .remaining = 100,
                          .market_volume = 0};
  EXPECT_EQ(s.on_slice(kT0, book.view(), st), 100);
}

TEST(ScheduledStrategy, IntegerRoundingConservesParentQuantity) {
  // X = 10 over 3 slices: children must be integers summing exactly to 10.
  ScheduledStrategy s("TWAP", twap_weights(3));
  const Qty executed =
      run_slices(s, parent(10, 3), [](int, Qty child) { return child; });
  EXPECT_EQ(executed, 10);
}

TEST(ScheduledStrategy, ValidatesConfiguration) {
  EXPECT_THROW(ScheduledStrategy("bad", {1.0, 0.5, 0.6, 0.0}),
               std::invalid_argument);  // not non-increasing
  EXPECT_THROW(ScheduledStrategy("bad", {0.9, 0.5, 0.0}),
               std::invalid_argument);  // does not start at 1
  EXPECT_THROW(ScheduledStrategy("bad", {1.0}), std::invalid_argument);

  ScheduledStrategy s("TWAP", twap_weights(4));
  EXPECT_THROW(s.on_start(parent(1000, 5)), std::invalid_argument);
  EXPECT_THROW(s.on_start(parent(0, 4)), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// POV
// ---------------------------------------------------------------------------

TEST(PovStrategy, PacesOnTrailingIntervalVolume) {
  PovStrategy s(0.1, /*cleanup_final_slice=*/false);
  const ParentOrder p = parent(10000, 4);
  BookFixture book;
  s.on_start(p);

  // Slice 0: no volume observed yet -> child 0.
  EXPECT_EQ(s.on_slice(kT0, book.view(),
                       {.slice_idx = 0, .remaining = 10000,
                        .market_volume = 0}),
            0);
  // Slice 1: 5000 traded in interval 0 -> child = 500.
  EXPECT_EQ(s.on_slice(kT0, book.view(),
                       {.slice_idx = 1, .remaining = 10000,
                        .market_volume = 5000}),
            500);
  // Slice 2: cumulative 8000 -> interval was 3000 -> child = 300.
  EXPECT_EQ(s.on_slice(kT0, book.view(),
                       {.slice_idx = 2, .remaining = 9500,
                        .market_volume = 8000}),
            300);
}

TEST(PovStrategy, CleanupForcesCompletionInFinalSlice) {
  PovStrategy s(0.05);  // cleanup defaults on
  const ParentOrder p = parent(10000, 3);
  BookFixture book;
  s.on_start(p);
  EXPECT_EQ(s.on_slice(kT0, book.view(),
                       {.slice_idx = 2, .remaining = 7777,
                        .market_volume = 100000}),
            7777);
}

TEST(PovStrategy, ChildNeverExceedsRemaining) {
  PovStrategy s(0.5, /*cleanup_final_slice=*/false);
  const ParentOrder p = parent(100, 4);
  BookFixture book;
  s.on_start(p);
  EXPECT_EQ(s.on_slice(kT0, book.view(),
                       {.slice_idx = 1, .remaining = 100,
                        .market_volume = 1000000}),
            100);
}

TEST(PovStrategy, ValidatesParticipation) {
  EXPECT_THROW(PovStrategy(0.0), std::invalid_argument);
  EXPECT_THROW(PovStrategy(1.0), std::invalid_argument);
  EXPECT_THROW(PovStrategy(-0.1), std::invalid_argument);
}

}  // namespace
}  // namespace oee
