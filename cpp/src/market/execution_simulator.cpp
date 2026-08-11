#include "oee/market/execution_simulator.hpp"

#include <algorithm>
#include <stdexcept>

namespace oee {

ExecutionSimulator::ExecutionSimulator(const LobsterDay& day,
                                       std::unique_ptr<IImpactModel> impact)
    : day_(&day), impact_(std::move(impact)) {
  if (!impact_) {
    throw std::invalid_argument("ExecutionSimulator: impact model is null");
  }
  if (day_->messages.rows() == 0) {
    throw std::invalid_argument("ExecutionSimulator: empty day");
  }
}

std::size_t ExecutionSimulator::row_at(Nanos t) const {
  const auto& ts = day_->messages.ts;
  if (t < ts.front()) {
    throw std::out_of_range(
        "ExecutionSimulator: t precedes the first book state");
  }
  // Last row with ts <= t. upper_bound gives the first row with ts > t;
  // one before that is ours. Timestamps are validated non-decreasing at
  // parse time, so this is well-defined.
  const auto it = std::upper_bound(ts.begin(), ts.end(), t);
  return static_cast<std::size_t>(it - ts.begin()) - 1;
}

ShiftedBookView ExecutionSimulator::book_at(Nanos t) const {
  return ShiftedBookView(day_->book, row_at(t), impact_->ladder_shift(t));
}

Fill ExecutionSimulator::execute_market(Nanos t, Side side, Qty qty) {
  if (qty <= 0) {
    throw std::invalid_argument("execute_market: qty must be > 0");
  }
  const ShiftedBookView view = book_at(t);

  Fill fill;
  fill.ts = t;
  fill.side = side;
  fill.requested = qty;
  fill.mid2x_before =
      (view.has_best_ask() && view.has_best_bid()) ? view.mid2x() : 0;

  // A sell consumes bids (walking down), a buy consumes asks (walking up).
  // LOBSTER levels are occupied and contiguous, so the first kNullPrice
  // level means visible depth is exhausted.
  Qty remaining = qty;
  for (int l = 0; l < view.levels() && remaining > 0; ++l) {
    const PriceTicks px = side == Side::kSell ? view.bid_px(l) : view.ask_px(l);
    const Qty depth    = side == Side::kSell ? view.bid_sz(l) : view.ask_sz(l);
    if (px == kNullPrice || depth <= 0) break;

    const Qty take = std::min(remaining, depth);
    fill.notional += static_cast<std::int64_t>(px) * take;
    fill.worst_px = px;
    fill.levels_touched += 1;
    fill.filled += take;
    remaining -= take;
  }

  if (fill.filled > 0) {
    impact_->on_fill(t, side == Side::kSell ? -fill.filled : fill.filled);
  }
  return fill;
}

}  // namespace oee
