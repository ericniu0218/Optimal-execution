#include "oee/data/trade_events.hpp"

#include <stdexcept>

namespace oee {
namespace {

bool is_execution(std::uint8_t type) {
  return type == static_cast<std::uint8_t>(MessageType::kExecuteVisible) ||
         type == static_cast<std::uint8_t>(MessageType::kExecuteHidden);
}

// Classify a hidden execution by price against the prevailing quotes
// (the book state BEFORE the event, i.e. the snapshot row preceding the
// first leg — snapshot row i is the state AFTER message i).
//
// Quote rule: at/above the ask => buyer-initiated; at/below the bid =>
// seller-initiated; strictly inside the spread, compare to the mid. An
// execution exactly at the mid falls through to the tick test, handled by
// the caller (needs trade-history state).
Aggressor classify_by_quotes(PriceTicks px, const BookTape& book,
                             std::size_t prev_row) {
  const bool have_ask = book.has_best_ask(prev_row);
  const bool have_bid = book.has_best_bid(prev_row);
  if (have_ask && px >= book.best_ask(prev_row)) return Aggressor::kBuyer;
  if (have_bid && px <= book.best_bid(prev_row)) return Aggressor::kSeller;
  if (have_ask && have_bid) {
    // Strictly inside the spread: compare 2*px to bid+ask (exact, integer).
    const PriceTicks mid2x = book.mid2x(prev_row);
    if (2 * px > mid2x) return Aggressor::kBuyer;
    if (2 * px < mid2x) return Aggressor::kSeller;
  }
  return Aggressor::kUnknown;  // at the mid, or one-sided book: caller tick-tests
}

}  // namespace

std::vector<TradeEvent> build_trade_events(const MessageTape& messages,
                                           const BookTape& book,
                                           const TradeEventOptions& opts) {
  if (messages.rows() != book.rows()) {
    throw std::runtime_error("build_trade_events: tape row counts differ");
  }

  std::vector<TradeEvent> events;

  // Tick-test state (fallback when the quote rule is inconclusive): the last
  // execution price seen, and the last successful classification.
  PriceTicks last_exec_px  = kNullPrice;
  Aggressor  last_aggressor = Aggressor::kUnknown;

  const std::size_t n = messages.rows();
  std::size_t i = 0;
  while (i < n) {
    if (!is_execution(messages.type[i])) {
      ++i;
      continue;
    }

    const bool hidden =
        messages.type[i] == static_cast<std::uint8_t>(MessageType::kExecuteHidden);

    // Aggressor for the visible case: opposite of the resting order's side.
    // LOBSTER direction = -1 means a resting sell was executed => buyer
    // aggressed. For hidden rows the direction field is unreliable; classify
    // from the pre-event book instead.
    Aggressor aggressor;
    if (hidden) {
      aggressor = (i > 0) ? classify_by_quotes(messages.price[i], book, i - 1)
                          : Aggressor::kUnknown;
      if (aggressor == Aggressor::kUnknown && last_exec_px != kNullPrice) {
        // Tick test: uptick => buyer, downtick => seller, zero tick => carry
        // the previous classification.
        if (messages.price[i] > last_exec_px)      aggressor = Aggressor::kBuyer;
        else if (messages.price[i] < last_exec_px) aggressor = Aggressor::kSeller;
        else                                       aggressor = last_aggressor;
      }
    } else {
      aggressor = messages.dir[i] == -1 ? Aggressor::kBuyer : Aggressor::kSeller;
    }

    TradeEvent ev{};
    ev.ts        = messages.ts[i];
    ev.aggressor = aggressor;
    ev.first_row = i;
    ev.hidden    = hidden;

    // Absorb consecutive legs of the same sweep: same execution type class,
    // same aggressor side, timestamp within tolerance of the FIRST leg.
    while (i < n && is_execution(messages.type[i])) {
      const bool leg_hidden =
          messages.type[i] == static_cast<std::uint8_t>(MessageType::kExecuteHidden);
      if (leg_hidden != hidden) break;
      if (messages.ts[i] - ev.ts > opts.group_tolerance) break;
      if (!hidden) {
        const Aggressor leg_agg =
            messages.dir[i] == -1 ? Aggressor::kBuyer : Aggressor::kSeller;
        if (leg_agg != aggressor) break;  // opposite-side trade in same ns
      }
      ev.total_qty += messages.size[i];
      ev.vwap_num  += messages.price[i] * messages.size[i];
      ev.num_legs  += 1;
      last_exec_px  = messages.price[i];
      ++i;
    }

    if (aggressor != Aggressor::kUnknown) last_aggressor = aggressor;
    events.push_back(ev);
  }
  return events;
}

}  // namespace oee
