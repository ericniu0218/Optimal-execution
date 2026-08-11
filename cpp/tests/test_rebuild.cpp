#include "oee/book/rebuild.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace oee {
namespace {

constexpr Nanos kT0 = 34200 * kNanosPerSec;
constexpr PriceTicks kNoAsk = kNullPrice;

// Build a 2-level synthetic day row by row: message + the snapshot state
// AFTER it, exactly as LOBSTER emits them.
struct RebuildDayBuilder {
  LobsterDay day;
  RebuildDayBuilder() { day.book.levels = 2; }

  RebuildDayBuilder& row(MessageType type, Qty size, PriceTicks px,
                         std::int8_t dir,
                         PriceTicks a1, Qty as1, PriceTicks a2, Qty as2,
                         PriceTicks b1, Qty bs1, PriceTicks b2, Qty bs2) {
    day.messages.ts.push_back(kT0 + static_cast<Nanos>(day.messages.rows()));
    day.messages.type.push_back(static_cast<std::uint8_t>(type));
    day.messages.order_id.push_back(
        static_cast<std::int64_t>(day.messages.rows()) + 1);
    day.messages.size.push_back(size);
    day.messages.price.push_back(px);
    day.messages.dir.push_back(dir);
    day.book.ask_price.insert(day.book.ask_price.end(), {a1, a2});
    day.book.ask_size.insert(day.book.ask_size.end(), {as1, as2});
    day.book.bid_price.insert(day.book.bid_price.end(), {b1, b2});
    day.book.bid_size.insert(day.book.bid_size.end(), {bs1, bs2});
    return *this;
  }
};

TEST(Rebuild, FullyObservedStreamMatchesExactly) {
  // Every order enters through the message stream; reconstruction is fully
  // determined, so every row must match with zero reseeds.
  RebuildDayBuilder b;
  //     type                          size  px       dir  ask1        ask2       bid1        bid2
  b.row(MessageType::kNewOrder,        100, 5000000,  1, kNoAsk, 0, kNoAsk, 0, 5000000, 100, kNullPrice, 0)
   .row(MessageType::kNewOrder,        200, 5001000, -1, 5001000, 200, kNoAsk, 0, 5000000, 100, kNullPrice, 0)
   .row(MessageType::kNewOrder,         50, 5000000,  1, 5001000, 200, kNoAsk, 0, 5000000, 150, kNullPrice, 0)
   .row(MessageType::kPartialCancel,    40, 5000000,  1, 5001000, 200, kNoAsk, 0, 5000000, 110, kNullPrice, 0)
   .row(MessageType::kExecuteVisible,   80, 5001000, -1, 5001000, 120, kNoAsk, 0, 5000000, 110, kNullPrice, 0)
   .row(MessageType::kExecuteHidden,    30, 5000500, -1, 5001000, 120, kNoAsk, 0, 5000000, 110, kNullPrice, 0)
   .row(MessageType::kDelete,           50, 5000000,  1, 5001000, 120, kNoAsk, 0, 5000000,  60, kNullPrice, 0)
   .row(MessageType::kNewOrder,         70, 5000900, -1, 5000900,  70, 5001000, 120, 5000000, 60, kNullPrice, 0);

  const RebuildStats st = validate_rebuild(b.day);
  EXPECT_EQ(st.rows, 7u);  // row 0 seeds
  EXPECT_EQ(st.exact_rows, 7u);
  EXPECT_EQ(st.surfaced_rows, 0u);
  EXPECT_EQ(st.hard_rows, 0u);
  EXPECT_EQ(st.clamped_deltas, 0u);
  EXPECT_TRUE(st.clean());
}

TEST(Rebuild, PreFileOrdersHandledViaSeeding) {
  // The bid at 5000000 rests from before the file window: no message ever
  // created it, but the row-0 seed carries it; its later deletion must
  // reconcile exactly.
  RebuildDayBuilder b;
  b.row(MessageType::kNewOrder, 200, 5001000, -1, 5001000, 200, kNoAsk, 0, 5000000, 100, kNullPrice, 0)
   .row(MessageType::kDelete,   100, 5000000,  1, 5001000, 200, kNoAsk, 0, kNullPrice, 0, kNullPrice, 0);

  const RebuildStats st = validate_rebuild(b.day);
  EXPECT_EQ(st.rows, 1u);
  EXPECT_EQ(st.exact_rows, 1u);
  EXPECT_TRUE(st.clean());
}

TEST(Rebuild, DeepLiquiditySurfacingIsBenignAndReseeds) {
  // Deleting the best bid reveals 4999800x75 — a level below the 2-level
  // visible range whose creation the message file never carried. That must
  // count as surfaced (not hard), and the reseeded level must participate
  // in later rows exactly.
  RebuildDayBuilder b;
  b.row(MessageType::kNewOrder, 200, 5001000, -1, 5001000, 200, kNoAsk, 0, 5000000, 100, 4999900, 50)
   .row(MessageType::kDelete,   100, 5000000,  1, 5001000, 200, kNoAsk, 0, 4999900,  50, 4999800, 75)
   .row(MessageType::kNewOrder,  10, 4999800,  1, 5001000, 200, kNoAsk, 0, 4999900,  50, 4999800, 85);

  const RebuildStats st = validate_rebuild(b.day);
  EXPECT_EQ(st.rows, 2u);
  EXPECT_EQ(st.surfaced_rows, 1u);
  EXPECT_EQ(st.surfaced_levels, 1u);
  EXPECT_EQ(st.exact_rows, 1u);  // the row AFTER the reseed is exact
  EXPECT_EQ(st.hard_rows, 0u);
  EXPECT_TRUE(st.clean());
}

TEST(Rebuild, CorruptedStreamIsDetectedAsHard) {
  // The snapshot claims the bid level holds 100 after an add that should
  // make it 150: the rebuild "knows" more than the exchange shows, which
  // is phantom liquidity — a genuine bug the validator must flag (this
  // test proves the validator is capable of failing).
  RebuildDayBuilder b;
  b.row(MessageType::kNewOrder, 100, 5000000, 1, kNoAsk, 0, kNoAsk, 0, 5000000, 100, kNullPrice, 0)
   .row(MessageType::kNewOrder,  50, 5000000, 1, kNoAsk, 0, kNoAsk, 0, 5000000, 100, kNullPrice, 0)
   .row(MessageType::kNewOrder,  10, 4999000, 1, kNoAsk, 0, kNoAsk, 0, 5000000, 100, 4999000, 10);

  const RebuildStats st = validate_rebuild(b.day);
  EXPECT_EQ(st.hard_rows, 1u);
  EXPECT_EQ(st.first_hard_row, 1);
  EXPECT_FALSE(st.clean());
  // Post-hard resync stops the fault from cascading: row 2 is exact.
  EXPECT_EQ(st.exact_rows, 1u);
}

TEST(Rebuild, PhantomLevelIsHard) {
  // A new order the snapshot does not show at all: phantom level.
  RebuildDayBuilder b;
  b.row(MessageType::kNewOrder, 100, 5000000, 1, kNoAsk, 0, kNoAsk, 0, 5000000, 100, kNullPrice, 0)
   .row(MessageType::kNewOrder,  20, 5000100, 1, kNoAsk, 0, kNoAsk, 0, 5000000, 100, kNullPrice, 0);

  const RebuildStats st = validate_rebuild(b.day);
  EXPECT_EQ(st.hard_rows, 1u);
  EXPECT_FALSE(st.clean());
}

TEST(Rebuild, ScrolledOutLevelsArePrunedNotPhantom) {
  // The regression found on real AAPL data: a level scrolls OUT of the
  // visible window when better quotes arrive, is modified out of view
  // (events omitted from a level-N file), then scrolls back smaller.
  // Without pruning the stale tracked size reads as phantom liquidity
  // (hard); with pruning its return is ordinary surfacing.
  RebuildDayBuilder b;
  //     type                     size  px       dir  ask1        ask2         bid1      bid2
  b.row(MessageType::kNewOrder,   100, 5001000, -1, 5001000, 100, 5002000, 50, 5000000, 100, kNullPrice, 0)
   // Better ask arrives: 5002000 leaves the 2-level window (prune it).
   .row(MessageType::kNewOrder,    30, 5000900, -1, 5000900,  30, 5001000, 100, 5000000, 100, kNullPrice, 0)
   .row(MessageType::kNewOrder,    20, 5000800, -1, 5000800,  20, 5000900,  30, 5000000, 100, kNullPrice, 0)
   // Range recedes: 5001000 was pruned when the window tightened past it
   // (row 2), so its return is surfacing — pruning is greedy at the
   // boundary because events beyond it are unobservable either way.
   .row(MessageType::kDelete,      30, 5000900, -1, 5000800,  20, 5001000, 100, 5000000, 100, kNullPrice, 0)
   // Recedes further: 5002000 returns SMALLER (an out-of-view cancel the
   // file never carried) => surfacing, not hard.
   .row(MessageType::kDelete,      20, 5000800, -1, 5001000, 100, 5002000,  35, 5000000, 100, kNullPrice, 0);

  const RebuildStats st = validate_rebuild(b.day);
  EXPECT_EQ(st.rows, 4u);
  EXPECT_EQ(st.hard_rows, 0u);       // the stale size is never phantom
  EXPECT_GE(st.pruned_levels, 2u);   // 5002000 at row 1, 5001000 at row 2
  EXPECT_EQ(st.surfaced_rows, 2u);   // both returns are reseeds
  EXPECT_EQ(st.exact_rows, 2u);
  EXPECT_TRUE(st.clean());
}

TEST(Rebuild, OverCancelClampsAndCounts) {
  // Cancel of 500 against a tracked 100 (remainder belongs to a pre-file
  // portion we cannot see... except we seeded row 0, so this is simply an
  // inconsistent delta): clamped, counted, and the snapshot agreement
  // still decides row status.
  RebuildDayBuilder b;
  b.row(MessageType::kNewOrder,      100, 5000000, 1, kNoAsk, 0, kNoAsk, 0, 5000000, 100, kNullPrice, 0)
   .row(MessageType::kPartialCancel, 500, 5000000, 1, kNoAsk, 0, kNoAsk, 0, kNullPrice, 0, kNullPrice, 0);

  const RebuildStats st = validate_rebuild(b.day);
  EXPECT_EQ(st.clamped_deltas, 1u);
  EXPECT_EQ(st.exact_rows, 1u);
  EXPECT_TRUE(st.clean());
}

}  // namespace
}  // namespace oee
