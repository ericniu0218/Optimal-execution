#include "oee/data/trade_events.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace oee {
namespace {

// Test tapes are built in memory (not via CSV) so each test can hit exactly
// the branch it targets without fixture noise.

struct TapeBuilder {
  MessageTape msgs;
  BookTape book;

  TapeBuilder() {
    book.levels = 1;  // only best bid/ask matter for event classification
  }

  // Append a message row plus the book state AFTER it.
  TapeBuilder& row(Nanos ts, MessageType type, Qty size, PriceTicks px,
                   std::int8_t dir, PriceTicks ask, Qty ask_sz, PriceTicks bid,
                   Qty bid_sz) {
    msgs.ts.push_back(ts);
    msgs.type.push_back(static_cast<std::uint8_t>(type));
    msgs.order_id.push_back(static_cast<std::int64_t>(msgs.rows()) + 1);
    msgs.size.push_back(size);
    msgs.price.push_back(px);
    msgs.dir.push_back(dir);
    book.ask_price.push_back(ask);
    book.ask_size.push_back(ask_sz);
    book.bid_price.push_back(bid);
    book.bid_size.push_back(bid_sz);
    return *this;
  }
};

constexpr Nanos kT0 = 34200 * kNanosPerSec;

TEST(TradeEvents, AggregatesMultiLevelSweep) {
  // Market buy for 300 sweeps two ask levels: two type-4 rows, same ns,
  // both with dir=-1 (resting sells hit).
  TapeBuilder b;
  b.row(kT0, MessageType::kNewOrder, 200, 5001000, -1, 5001000, 200, 5000000, 100)
      .row(kT0 + 10, MessageType::kNewOrder, 150, 5002000, -1, 5001000, 200, 5000000, 100)
      .row(kT0 + 20, MessageType::kExecuteVisible, 200, 5001000, -1, 5002000, 150, 5000000, 100)
      .row(kT0 + 20, MessageType::kExecuteVisible, 100, 5002000, -1, 5002000, 50, 5000000, 100);

  const auto events = build_trade_events(b.msgs, b.book);
  ASSERT_EQ(events.size(), 1u);
  const TradeEvent& ev = events[0];
  EXPECT_EQ(ev.total_qty, 300);
  EXPECT_EQ(ev.num_legs, 2);
  EXPECT_EQ(ev.aggressor, Aggressor::kBuyer);  // resting sells hit => buyer
  EXPECT_EQ(ev.first_row, 2u);
  EXPECT_FALSE(ev.hidden);
  // VWAP = (200*5001000 + 100*5002000)/300
  EXPECT_EQ(ev.vwap_num, 200LL * 5001000 + 100LL * 5002000);
  EXPECT_NEAR(ev.vwap_ticks(), 5001333.333333, 1e-6);
}

TEST(TradeEvents, OppositeSideSameNanosecondSplits) {
  // A buy-side and a sell-side execution in the same ns are different events.
  TapeBuilder b;
  b.row(kT0, MessageType::kExecuteVisible, 100, 5001000, -1, 5001000, 100, 5000000, 100)
      .row(kT0, MessageType::kExecuteVisible, 50, 5000000, 1, 5001000, 100, 5000000, 50);

  const auto events = build_trade_events(b.msgs, b.book);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].aggressor, Aggressor::kBuyer);
  EXPECT_EQ(events[1].aggressor, Aggressor::kSeller);
}

TEST(TradeEvents, InterleavedMessageBreaksGroup) {
  // Same ts and side, but a cancel lands between the two executions: the book
  // changed mid-stream, so they are separate events.
  TapeBuilder b;
  b.row(kT0, MessageType::kExecuteVisible, 100, 5001000, -1, 5001000, 50, 5000000, 100)
      .row(kT0, MessageType::kDelete, 50, 5001000, -1, 5002000, 150, 5000000, 100)
      .row(kT0, MessageType::kExecuteVisible, 80, 5002000, -1, 5002000, 70, 5000000, 100);

  const auto events = build_trade_events(b.msgs, b.book);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].total_qty, 100);
  EXPECT_EQ(events[1].total_qty, 80);
}

TEST(TradeEvents, GroupToleranceIsAnchoredToFirstLeg) {
  // Legs at t, t+500, t+900 with tolerance 1000: all one event.
  // With tolerance 0: three events.
  TapeBuilder b;
  b.row(kT0, MessageType::kExecuteVisible, 10, 5001000, -1, 5001000, 90, 5000000, 100)
      .row(kT0 + 500, MessageType::kExecuteVisible, 10, 5001000, -1, 5001000, 80, 5000000, 100)
      .row(kT0 + 900, MessageType::kExecuteVisible, 10, 5001000, -1, 5001000, 70, 5000000, 100);

  TradeEventOptions tol1000{.group_tolerance = 1000};
  EXPECT_EQ(build_trade_events(b.msgs, b.book, tol1000).size(), 1u);
  EXPECT_EQ(build_trade_events(b.msgs, b.book).size(), 3u);

  // Anchoring: legs at t, t+900, t+1800 with tolerance 1000 must NOT
  // daisy-chain — the third leg is beyond the FIRST leg's window.
  TapeBuilder c;
  c.row(kT0, MessageType::kExecuteVisible, 10, 5001000, -1, 5001000, 90, 5000000, 100)
      .row(kT0 + 900, MessageType::kExecuteVisible, 10, 5001000, -1, 5001000, 80, 5000000, 100)
      .row(kT0 + 1800, MessageType::kExecuteVisible, 10, 5001000, -1, 5001000, 70, 5000000, 100);
  const auto chained = build_trade_events(c.msgs, c.book, tol1000);
  ASSERT_EQ(chained.size(), 2u);
  EXPECT_EQ(chained[0].num_legs, 2);
  EXPECT_EQ(chained[1].num_legs, 1);
}

TEST(TradeEvents, HiddenClassifiedByQuoteRule) {
  // Book before each hidden exec: bid 5000000, ask 5002000 (mid 5001000).
  TapeBuilder b;
  b.row(kT0, MessageType::kNewOrder, 100, 5002000, -1, 5002000, 100, 5000000, 100)
      // At the ask => buyer-initiated.
      .row(kT0 + 10, MessageType::kExecuteHidden, 10, 5002000, -1, 5002000, 100, 5000000, 100)
      // At the bid => seller-initiated.
      .row(kT0 + 20, MessageType::kExecuteHidden, 10, 5000000, -1, 5002000, 100, 5000000, 100)
      // Inside spread, above mid => buyer.
      .row(kT0 + 30, MessageType::kExecuteHidden, 10, 5001500, -1, 5002000, 100, 5000000, 100)
      // Inside spread, below mid => seller.
      .row(kT0 + 40, MessageType::kExecuteHidden, 10, 5000500, -1, 5002000, 100, 5000000, 100);

  const auto events = build_trade_events(b.msgs, b.book);
  ASSERT_EQ(events.size(), 4u);
  EXPECT_TRUE(events[0].hidden);
  EXPECT_EQ(events[0].aggressor, Aggressor::kBuyer);
  EXPECT_EQ(events[1].aggressor, Aggressor::kSeller);
  EXPECT_EQ(events[2].aggressor, Aggressor::kBuyer);
  EXPECT_EQ(events[3].aggressor, Aggressor::kSeller);
}

TEST(TradeEvents, HiddenAtMidFallsBackToTickTest) {
  // Exec exactly at the mid: quote rule is inconclusive, tick test decides.
  // Prior exec at 5000500, hidden exec at mid 5001000 => uptick => buyer.
  TapeBuilder b;
  b.row(kT0, MessageType::kExecuteVisible, 10, 5000500, 1, 5002000, 100, 5000000, 100)
      .row(kT0 + 10, MessageType::kExecuteHidden, 10, 5001000, -1, 5002000, 100, 5000000, 100)
      // Zero tick at the mid: carries the previous classification (buyer).
      .row(kT0 + 20, MessageType::kExecuteHidden, 10, 5001000, -1, 5002000, 100, 5000000, 100);

  const auto events = build_trade_events(b.msgs, b.book);
  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events[1].aggressor, Aggressor::kBuyer);   // uptick
  EXPECT_EQ(events[2].aggressor, Aggressor::kBuyer);   // zero tick carry
}

TEST(TradeEvents, HiddenNeverMergesWithVisible) {
  // Visible and hidden legs in the same ns stay separate events.
  TapeBuilder b;
  b.row(kT0, MessageType::kExecuteVisible, 100, 5001000, -1, 5001000, 50, 5000000, 100)
      .row(kT0, MessageType::kExecuteHidden, 40, 5001000, -1, 5001000, 50, 5000000, 100);

  const auto events = build_trade_events(b.msgs, b.book);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_FALSE(events[0].hidden);
  EXPECT_TRUE(events[1].hidden);
}

TEST(TradeEvents, NonExecutionRowsProduceNoEvents) {
  TapeBuilder b;
  b.row(kT0, MessageType::kNewOrder, 100, 5001000, -1, 5001000, 100, 5000000, 100)
      .row(kT0 + 10, MessageType::kPartialCancel, 50, 5001000, -1, 5001000, 50, 5000000, 100)
      .row(kT0 + 20, MessageType::kDelete, 50, 5001000, -1, kNullPrice, 0, 5000000, 100);

  EXPECT_TRUE(build_trade_events(b.msgs, b.book).empty());
}

}  // namespace
}  // namespace oee
