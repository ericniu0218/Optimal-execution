#include "oee/market/execution_simulator.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

namespace oee {
namespace {

constexpr Nanos kT0 = 34200 * kNanosPerSec;

// Build a synthetic day with a static 3-level book repeated at each event
// time. Message rows are minimal type-1 stubs — the simulator only consumes
// timestamps and book states.
//
//   asks: 5000100x100, 5000200x200, 5000300x300
//   bids: 5000000x100, 4999900x200, 4999800x300   (total visible: 600/side)
LobsterDay static_day(int n_rows, Nanos dt = kNanosPerSec) {
  LobsterDay day;
  day.book.levels = 3;
  for (int i = 0; i < n_rows; ++i) {
    day.messages.ts.push_back(kT0 + i * dt);
    day.messages.type.push_back(1);
    day.messages.order_id.push_back(i + 1);
    day.messages.size.push_back(1);
    day.messages.price.push_back(5000000);
    day.messages.dir.push_back(1);

    day.book.ask_price.insert(day.book.ask_price.end(),
                              {5000100, 5000200, 5000300});
    day.book.ask_size.insert(day.book.ask_size.end(), {100, 200, 300});
    day.book.bid_price.insert(day.book.bid_price.end(),
                              {5000000, 4999900, 4999800});
    day.book.bid_size.insert(day.book.bid_size.end(), {100, 200, 300});
  }
  return day;
}

ExecutionSimulator make_sim(const LobsterDay& day, double gamma = 0.0) {
  return ExecutionSimulator(
      day, gamma == 0.0
               ? std::unique_ptr<IImpactModel>(std::make_unique<NoImpact>())
               : std::make_unique<LinearPermanentImpact>(gamma));
}

TEST(ExecutionSimulator, RowAtBinarySearch) {
  const LobsterDay day = static_day(3);  // rows at kT0, +1s, +2s
  ExecutionSimulator sim = make_sim(day);

  EXPECT_EQ(sim.row_at(kT0), 0u);                        // exactly at a row
  EXPECT_EQ(sim.row_at(kT0 + kNanosPerSec / 2), 0u);     // between rows
  EXPECT_EQ(sim.row_at(kT0 + kNanosPerSec), 1u);
  EXPECT_EQ(sim.row_at(kT0 + 100 * kNanosPerSec), 2u);   // after last row
  EXPECT_THROW(sim.row_at(kT0 - 1), std::out_of_range);  // before the book
}

TEST(ExecutionSimulator, OneShareFillsAtTouch) {
  const LobsterDay day = static_day(1);
  ExecutionSimulator sim = make_sim(day);

  const Fill sell = sim.execute_market(kT0, Side::kSell, 1);
  EXPECT_EQ(sell.filled, 1);
  EXPECT_EQ(sell.notional, 5000000);
  EXPECT_EQ(sell.levels_touched, 1);
  EXPECT_EQ(sell.worst_px, 5000000);

  const Fill buy = sim.execute_market(kT0, Side::kBuy, 1);
  EXPECT_EQ(buy.notional, 5000100);
  EXPECT_EQ(buy.mid2x_before, 5000100 + 5000000);
}

TEST(ExecutionSimulator, MultiLevelWalkComputesExactVwap) {
  const LobsterDay day = static_day(1);
  ExecutionSimulator sim = make_sim(day);

  // Sell 250: 100 @ 5000000, then 150 @ 4999900.
  const Fill f = sim.execute_market(kT0, Side::kSell, 250);
  EXPECT_EQ(f.filled, 250);
  EXPECT_EQ(f.notional, 100LL * 5000000 + 150LL * 4999900);
  EXPECT_EQ(f.levels_touched, 2);
  EXPECT_EQ(f.worst_px, 4999900);
  EXPECT_DOUBLE_EQ(f.vwap_ticks(),
                   (100.0 * 5000000 + 150.0 * 4999900) / 250.0);
}

TEST(ExecutionSimulator, PartialFillWhenVisibleDepthExhausted) {
  const LobsterDay day = static_day(1);
  ExecutionSimulator sim = make_sim(day);

  // Only 600 shares of visible bid depth exist.
  const Fill f = sim.execute_market(kT0, Side::kSell, 1000);
  EXPECT_EQ(f.requested, 1000);
  EXPECT_EQ(f.filled, 600);
  EXPECT_EQ(f.levels_touched, 3);
  EXPECT_EQ(f.worst_px, 4999800);
  EXPECT_EQ(f.notional,
            100LL * 5000000 + 200LL * 4999900 + 300LL * 4999800);
}

TEST(ExecutionSimulator, PermanentImpactShiftsSubsequentFills) {
  const LobsterDay day = static_day(10);
  // gamma = 0.01 ticks/share: selling 500 shifts the ladder by -5 ticks.
  ExecutionSimulator sim = make_sim(day, 0.01);

  const Fill first = sim.execute_market(kT0, Side::kSell, 500);
  EXPECT_EQ(first.filled, 500);
  // First fill sees the unshifted ladder (no prior impact).
  EXPECT_EQ(first.vwap_ticks(),
            (100.0 * 5000000 + 200.0 * 4999900 + 200.0 * 4999800) / 500.0);

  // The strategy's view of the market is now shifted down 5 ticks...
  const ShiftedBookView view = sim.book_at(kT0 + kNanosPerSec);
  EXPECT_EQ(view.shift(), -5);
  EXPECT_EQ(view.best_bid(), 5000000 - 5);
  EXPECT_EQ(view.best_ask(), 5000100 - 5);
  // ...but depth is unchanged (documented approximation: prices move,
  // shape does not).
  EXPECT_EQ(view.bid_sz(0), 100);

  // And the next fill executes on the shifted ladder.
  const Fill second = sim.execute_market(kT0 + kNanosPerSec, Side::kSell, 1);
  EXPECT_EQ(second.notional, 5000000 - 5);
}

TEST(ExecutionSimulator, SequentialChildrenGetMonotonicallyWorsePrices) {
  // The committed exit test: with a static replayed book, each successive
  // child of a sell program must realize a strictly worse VWAP than the
  // previous one, because accumulated permanent impact has shifted the
  // ladder down in between. Without impact feedback all five would fill
  // identically — the book "magically refills".
  const LobsterDay day = static_day(10);
  ExecutionSimulator sim = make_sim(day, 0.05);  // 100 shares -> 5 ticks

  double prev_vwap = 1e18;
  for (int k = 0; k < 5; ++k) {
    const Fill f =
        sim.execute_market(kT0 + k * kNanosPerSec, Side::kSell, 100);
    EXPECT_EQ(f.filled, 100);
    EXPECT_LT(f.vwap_ticks(), prev_vwap) << "child " << k;
    prev_vwap = f.vwap_ticks();
  }

  // Total ladder shift after 500 shares at gamma = 0.05: -25 ticks.
  EXPECT_EQ(sim.impact().ladder_shift(kT0 + 100 * kNanosPerSec), -25);
}

TEST(ExecutionSimulator, BuysShiftLadderUpSymmetrically) {
  const LobsterDay day = static_day(10);
  ExecutionSimulator sim = make_sim(day, 0.01);

  sim.execute_market(kT0, Side::kBuy, 500);
  EXPECT_EQ(sim.impact().ladder_shift(kT0 + 100 * kNanosPerSec), +5);
  EXPECT_EQ(sim.book_at(kT0 + kNanosPerSec).best_ask(), 5000100 + 5);
}

TEST(ExecutionSimulator, ImpactRoundsCumulativeNotPerFill) {
  // gamma = 0.004: each 100-share fill contributes 0.4 ticks. Rounding per
  // fill would floor every contribution to zero; rounding the cumulative
  // product must yield llround(1.2) = 1 tick after three fills.
  const LobsterDay day = static_day(10);
  ExecutionSimulator sim = make_sim(day, 0.004);

  for (int k = 0; k < 3; ++k) {
    sim.execute_market(kT0 + k * kNanosPerSec, Side::kSell, 100);
  }
  EXPECT_EQ(sim.impact().ladder_shift(kT0 + 100 * kNanosPerSec), -1);
}

TEST(ExecutionSimulator, EmptyLevelsDoNotShift) {
  // A one-sided book: kNullPrice levels must stay kNullPrice in the shifted
  // view, not become kNullPrice + shift.
  LobsterDay day;
  day.book.levels = 1;
  day.messages.ts.push_back(kT0);
  day.messages.type.push_back(1);
  day.messages.order_id.push_back(1);
  day.messages.size.push_back(1);
  day.messages.price.push_back(5000000);
  day.messages.dir.push_back(1);
  day.book.ask_price.push_back(kNullPrice);  // no ask
  day.book.ask_size.push_back(0);
  day.book.bid_price.push_back(5000000);
  day.book.bid_size.push_back(100);

  ExecutionSimulator sim = make_sim(day, 0.01);
  sim.execute_market(kT0, Side::kSell, 100);  // create a nonzero shift
  const ShiftedBookView view = sim.book_at(kT0);
  EXPECT_EQ(view.ask_px(0), kNullPrice);
  EXPECT_FALSE(view.has_best_ask());
  EXPECT_NE(view.best_bid(), 5000000);  // real level did shift

  // A buy against the empty ask side fills nothing and moves nothing.
  const PriceTicks shift_before = sim.impact().ladder_shift(kT0 + 100 * kNanosPerSec);
  const Fill f = sim.execute_market(kT0, Side::kBuy, 10);
  EXPECT_EQ(f.filled, 0);
  EXPECT_EQ(f.notional, 0);
  EXPECT_EQ(sim.impact().ladder_shift(kT0 + 100 * kNanosPerSec), shift_before);
}

TEST(ExecutionSimulator, RejectsInvalidOrdersAndConstruction) {
  const LobsterDay day = static_day(1);
  ExecutionSimulator sim = make_sim(day);
  EXPECT_THROW(sim.execute_market(kT0, Side::kSell, 0), std::invalid_argument);
  EXPECT_THROW(sim.execute_market(kT0, Side::kSell, -5), std::invalid_argument);
  EXPECT_THROW(sim.execute_market(kT0 - 1, Side::kSell, 1), std::out_of_range);

  EXPECT_THROW(ExecutionSimulator(day, nullptr), std::invalid_argument);
  const LobsterDay empty;
  EXPECT_THROW(ExecutionSimulator(empty, std::make_unique<NoImpact>()),
               std::invalid_argument);
}

}  // namespace
}  // namespace oee
