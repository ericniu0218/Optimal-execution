#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "oee/core/types.hpp"
#include "oee/data/message_tape.hpp"
#include "oee/market/impact_model.hpp"

namespace oee {

// ---------------------------------------------------------------------------
// Execution against the replayed book.
//
// The simulator answers two questions for a strategy:
//   1. "What does the market look like at time t?"  -> book_at(t)
//   2. "What happens if I send a marketable order?" -> execute_market(...)
//
// Both are answered through the impact model's ladder shift, so a strategy
// never sees (or trades against) the pristine historical prices once it has
// started moving the market. All child orders are marketable — this project
// deliberately models no passive orders / queue position (AC assumes a trade
// rate, not a fill probability).
// ---------------------------------------------------------------------------

// Read-only view of one replay row with the strategy's own permanent impact
// applied as a uniform shift to every occupied price level. Empty levels
// stay kNullPrice — "no price" does not shift.
class ShiftedBookView {
 public:
  ShiftedBookView(const BookTape& tape, std::size_t row, PriceTicks shift)
      : tape_(&tape), row_(row), shift_(shift) {}

  int levels() const { return tape_->levels; }
  std::size_t row() const { return row_; }
  PriceTicks shift() const { return shift_; }

  PriceTicks ask_px(int level) const {
    const PriceTicks p = tape_->ask_px(row_, level);
    return p == kNullPrice ? kNullPrice : p + shift_;
  }
  PriceTicks bid_px(int level) const {
    const PriceTicks p = tape_->bid_px(row_, level);
    return p == kNullPrice ? kNullPrice : p + shift_;
  }
  Qty ask_sz(int level) const { return tape_->ask_sz(row_, level); }
  Qty bid_sz(int level) const { return tape_->bid_sz(row_, level); }

  bool has_best_ask() const { return tape_->has_best_ask(row_); }
  bool has_best_bid() const { return tape_->has_best_bid(row_); }
  PriceTicks best_ask() const { return ask_px(0); }
  PriceTicks best_bid() const { return bid_px(0); }

  // 2x mid (integer-exact); only meaningful when both sides exist.
  PriceTicks mid2x() const { return best_ask() + best_bid(); }

 private:
  const BookTape* tape_;
  std::size_t row_;
  PriceTicks shift_;
};

// One executed child order. All prices are on the SHIFTED ladder — this is
// what the strategy actually paid/received in the simulated world.
struct Fill {
  Nanos ts = 0;
  Side side = Side::kSell;
  Qty requested = 0;
  Qty filled = 0;                // < requested iff visible depth ran out
  std::int64_t notional = 0;     // sum(px * qty), ticks * shares
  PriceTicks worst_px = 0;       // deepest level touched
  int levels_touched = 0;
  PriceTicks mid2x_before = 0;   // 2x mid just before execution; 0 if book
                                 // was one-sided at that instant

  double vwap_ticks() const {
    return filled > 0 ? static_cast<double>(notional) / static_cast<double>(filled)
                      : 0.0;
  }
};

class ExecutionSimulator {
 public:
  // Both references must outlive the simulator. The impact model is owned.
  ExecutionSimulator(const LobsterDay& day, std::unique_ptr<IImpactModel> impact);

  // Index of the last replay row with ts <= t (the book state prevailing at
  // t). Throws std::out_of_range if t precedes the first row — executing
  // before the book exists is a backtest wiring bug, not a market condition.
  std::size_t row_at(Nanos t) const;

  // The market as the strategy is allowed to see it at time t.
  ShiftedBookView book_at(Nanos t) const;

  // Execute a marketable child order against the book prevailing at t:
  // walk visible depth level by level on the shifted ladder. Fills partially
  // if L10 visible depth runs out — the unfilled remainder simply stays in
  // the caller's inventory (no hidden liquidity, conservatively biased).
  // Updates the impact model with the filled quantity.
  Fill execute_market(Nanos t, Side side, Qty qty);

  const IImpactModel& impact() const { return *impact_; }

 private:
  const LobsterDay* day_;
  std::unique_ptr<IImpactModel> impact_;
};

}  // namespace oee
