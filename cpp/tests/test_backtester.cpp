#include "oee/backtest/backtester.hpp"

#include <gtest/gtest.h>

#include <memory>

#include "oee/strategy/pov.hpp"
#include "oee/strategy/scheduled.hpp"

namespace oee {
namespace {

constexpr Nanos kT0 = 34200 * kNanosPerSec;
constexpr Nanos kSec = kNanosPerSec;

// Synthetic-day builder. Each row appends a message and a 3-level book
// state; helpers add plain "book refresh" rows or visible executions
// (which become market volume via trade-event aggregation).
struct DayBuilder {
  LobsterDay day;
  DayBuilder() { day.book.levels = 3; }

  // Book with best bid `bid` / best ask `bid+100`, deeper levels 100 ticks
  // apart with size `depth` each.
  DayBuilder& book_row(Nanos ts, PriceTicks bid, Qty depth,
                       std::uint8_t type = 1, Qty size = 1,
                       PriceTicks msg_px = 0, std::int8_t dir = 1) {
    day.messages.ts.push_back(ts);
    day.messages.type.push_back(type);
    day.messages.order_id.push_back(
        static_cast<std::int64_t>(day.messages.rows()) + 1);
    day.messages.size.push_back(size);
    day.messages.price.push_back(msg_px == 0 ? bid : msg_px);
    day.messages.dir.push_back(dir);
    day.book.ask_price.insert(day.book.ask_price.end(),
                              {bid + 100, bid + 200, bid + 300});
    day.book.ask_size.insert(day.book.ask_size.end(), {depth, depth, depth});
    day.book.bid_price.insert(day.book.bid_price.end(),
                              {bid, bid - 100, bid - 200});
    day.book.bid_size.insert(day.book.bid_size.end(), {depth, depth, depth});
    return *this;
  }

  // A visible execution of `qty` (buyer-initiated) => market volume.
  DayBuilder& trade_row(Nanos ts, PriceTicks bid, Qty depth, Qty qty) {
    return book_row(ts, bid, depth, /*type=*/4, qty, bid + 100, /*dir=*/-1);
  }
};

// Static book: bid 5000000, generous depth, rows every second.
LobsterDay static_day(int rows, Qty depth = 1'000'000) {
  DayBuilder b;
  for (int i = 0; i < rows; ++i) b.book_row(kT0 + i * kSec, 5000000, depth);
  return b.day;
}

ParentOrder sell_parent(Qty qty, int slices, Nanos start = kT0,
                        Nanos end = kT0 + 4 * kSec) {
  return ParentOrder{.side = Side::kSell, .quantity = qty, .start = start,
                     .end = end, .num_slices = slices};
}

// ---------------------------------------------------------------------------

TEST(Backtester, TwapOnStaticBookCostsExactlyHalfSpread) {
  const LobsterDay day = static_day(10);
  Backtester bt(day);
  ScheduledStrategy twap("TWAP", twap_weights(4));

  const BacktestResult r =
      bt.run(twap, sell_parent(400, 4), std::make_unique<NoImpact>());

  EXPECT_EQ(r.executed, 400);
  EXPECT_EQ(r.remainder, 0);
  EXPECT_DOUBLE_EQ(r.arrival_mid_ticks, 5000050.0);
  // Every child fills at the touch: IS = 400 shares * half-spread (50).
  EXPECT_DOUBLE_EQ(r.is_ticks_shares, 400.0 * 50.0);
  EXPECT_DOUBLE_EQ(r.cost_execution, 400.0 * 50.0);
  EXPECT_DOUBLE_EQ(r.cost_drift, 0.0);
  EXPECT_DOUBLE_EQ(r.cost_permanent, 0.0);
  EXPECT_DOUBLE_EQ(r.cost_opportunity, 0.0);
  EXPECT_NEAR(r.is_bps, 50.0 / 5000050.0 * 1e4, 1e-9);
}

TEST(Backtester, PermanentImpactDecompositionHandChecked) {
  // TWAP 400 over 4 slices, gamma = 0.01 ticks/share, L1 depth exactly 100
  // so each 100-share child consumes L1 at the shifted touch.
  // Shifts at fill time: 0, -1, -2, -3.
  //   permanent = 100*(0+1+2+3)          = 600
  //   execution = 4 * 100 * half-spread  = 20000
  //   IS        = 20600 exactly.
  DayBuilder b;
  for (int i = 0; i < 10; ++i) b.book_row(kT0 + i * kSec, 5000000, 100);
  Backtester bt(b.day);
  ScheduledStrategy twap("TWAP", twap_weights(4));

  const BacktestResult r = bt.run(twap, sell_parent(400, 4),
                                  std::make_unique<LinearPermanentImpact>(0.01));

  EXPECT_EQ(r.executed, 400);
  EXPECT_DOUBLE_EQ(r.cost_permanent, 600.0);
  EXPECT_DOUBLE_EQ(r.cost_execution, 20000.0);
  EXPECT_DOUBLE_EQ(r.cost_drift, 0.0);
  EXPECT_DOUBLE_EQ(r.is_ticks_shares, 20600.0);
  // Terminal mid carries the full -4-tick shift (400 shares * 0.01).
  EXPECT_DOUBLE_EQ(r.terminal_mid_ticks, 5000050.0 - 4.0);
}

TEST(Backtester, DecompositionSumsToTotalOnDriftingBook) {
  // Rising market (+10 ticks/row) with impact: every component is active.
  // The identity IS == drift + permanent + execution + opportunity must
  // hold to numerical precision — it is a theorem about the accounting,
  // and this is its proof-by-assertion on an awkward path.
  DayBuilder b;
  for (int i = 0; i < 10; ++i) {
    b.book_row(kT0 + i * kSec, 5000000 + 10 * i, 150);
  }
  Backtester bt(b.day);
  ScheduledStrategy twap("TWAP", twap_weights(4));

  const BacktestResult r = bt.run(twap, sell_parent(500, 4),
                                  std::make_unique<LinearPermanentImpact>(0.02));

  EXPECT_EQ(r.executed, 500);
  EXPECT_NEAR(r.is_ticks_shares,
              r.cost_drift + r.cost_permanent + r.cost_execution +
                  r.cost_opportunity,
              1e-6);
  // Selling into a rising market: drift is a NEGATIVE cost (a gain).
  EXPECT_LT(r.cost_drift, 0.0);
  EXPECT_GT(r.cost_permanent, 0.0);
  EXPECT_GT(r.cost_execution, 0.0);
}

TEST(Backtester, UnfilledRemainderValuedAtTerminalMid) {
  // One slice, thin book: child 1000 vs 300 total visible depth. The 700
  // unexecuted shares are marked at the terminal (shifted) mid — Perold
  // opportunity cost, exercised on a falling market so it is a real cost.
  DayBuilder b;
  b.book_row(kT0, 5000000, 100);
  b.book_row(kT0 + 4 * kSec, 4999000, 100);  // market fell 1000 ticks
  Backtester bt(b.day);
  ScheduledStrategy send_all("ALL", twap_weights(1));

  const BacktestResult r = bt.run(send_all, sell_parent(1000, 1),
                                  std::make_unique<NoImpact>());

  EXPECT_EQ(r.executed, 300);
  EXPECT_EQ(r.remainder, 700);
  EXPECT_DOUBLE_EQ(r.terminal_mid_ticks, 4999050.0);
  EXPECT_DOUBLE_EQ(r.cost_opportunity, 700.0 * (5000050.0 - 4999050.0));
  EXPECT_NEAR(r.is_ticks_shares,
              r.cost_drift + r.cost_permanent + r.cost_execution +
                  r.cost_opportunity,
              1e-6);
}

TEST(Backtester, BuySideSignsAreSymmetric) {
  const LobsterDay day = static_day(10);
  Backtester bt(day);
  ScheduledStrategy twap("TWAP", twap_weights(4));
  const ParentOrder buy{.side = Side::kBuy, .quantity = 400, .start = kT0,
                        .end = kT0 + 4 * kSec, .num_slices = 4};

  const BacktestResult r = bt.run(twap, buy, std::make_unique<NoImpact>());

  // Paying the half-spread is a positive cost on the buy side too.
  EXPECT_DOUBLE_EQ(r.is_ticks_shares, 400.0 * 50.0);
  EXPECT_DOUBLE_EQ(r.cost_execution, 400.0 * 50.0);
}

TEST(Backtester, MarketVolumeFeedDrivesPov) {
  // Trades: 5000 shares in interval [0,1s), 3000 in [1s,2s), none later.
  DayBuilder b;
  b.book_row(kT0, 5000000, 1'000'000);
  b.trade_row(kT0 + kSec / 2, 5000000, 1'000'000, 5000);
  b.book_row(kT0 + kSec, 5000000, 1'000'000);
  b.trade_row(kT0 + kSec + kSec / 2, 5000000, 1'000'000, 3000);
  b.book_row(kT0 + 2 * kSec, 5000000, 1'000'000);
  b.book_row(kT0 + 3 * kSec, 5000000, 1'000'000);
  b.book_row(kT0 + 4 * kSec, 5000000, 1'000'000);
  Backtester bt(b.day);

  EXPECT_EQ(bt.market_volume_between(kT0, kT0 + kSec), 5000);
  EXPECT_EQ(bt.market_volume_between(kT0 + kSec, kT0 + 2 * kSec), 3000);
  EXPECT_EQ(bt.market_volume_between(kT0, kT0 + 4 * kSec), 8000);

  PovStrategy pov(0.1, /*cleanup_final_slice=*/false);
  const BacktestResult r =
      bt.run(pov, sell_parent(10000, 4), std::make_unique<NoImpact>());

  // Slice children: 0 (no volume yet), 500 (10% of 5000), 300 (10% of
  // 3000), 0 (no volume in [2s,3s)). POV without cleanup does not complete.
  ASSERT_EQ(r.slices.size(), 4u);
  EXPECT_EQ(r.slices[0].child, 0);
  EXPECT_EQ(r.slices[1].child, 500);
  EXPECT_EQ(r.slices[2].child, 300);
  EXPECT_EQ(r.slices[3].child, 0);
  EXPECT_EQ(r.executed, 800);
  EXPECT_EQ(r.remainder, 9200);
  EXPECT_NEAR(r.participation, 800.0 / 8000.0, 1e-12);
}

TEST(Backtester, StrategyReturningOversizedChildIsALogicError) {
  struct Rogue final : IExecutionStrategy {
    std::string name() const override { return "rogue"; }
    void on_start(const ParentOrder&) override {}
    Qty on_slice(Nanos, const ShiftedBookView&,
                 const ExecutionState& st) override {
      return st.remaining + 1;
    }
  };
  const LobsterDay day = static_day(10);
  Backtester bt(day);
  Rogue rogue;
  EXPECT_THROW(
      bt.run(rogue, sell_parent(100, 2), std::make_unique<NoImpact>()),
      std::logic_error);
}

TEST(Backtester, ValidatesParentOrder) {
  const LobsterDay day = static_day(10);
  Backtester bt(day);
  ScheduledStrategy twap("TWAP", twap_weights(2));
  EXPECT_THROW(bt.run(twap, sell_parent(0, 2), std::make_unique<NoImpact>()),
               std::invalid_argument);
  EXPECT_THROW(bt.run(twap, sell_parent(100, 2, kT0 + kSec, kT0),
                      std::make_unique<NoImpact>()),
               std::invalid_argument);
}

}  // namespace
}  // namespace oee
