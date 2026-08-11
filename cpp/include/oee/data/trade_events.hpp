#pragma once

#include <cstddef>
#include <vector>

#include "oee/data/message_tape.hpp"

namespace oee {

// ---------------------------------------------------------------------------
// Trade-event aggregation.
//
// When a marketable order sweeps K resting orders, LOBSTER writes K separate
// type-4 rows sharing one timestamp (one row per matched limit order, walking
// up/down the book). Treating each row as an independent trade misstates both
// trade sizes and per-trade price impact, which silently corrupts the gamma /
// eta regressions downstream. This module groups those legs back into single
// logical trade events before anything else consumes them.
//
// Hidden executions (type 5) never touch the visible ladder and their
// direction field is unreliable in the raw data. They are kept as separate
// events (never merged with visible legs) and classified by the quote rule /
// tick test (Lee & Ready 1991) against the book state prevailing BEFORE the
// event. Calibration can then include or exclude them explicitly.
// ---------------------------------------------------------------------------

struct TradeEvent {
  Nanos       ts;          // timestamp of the first leg
  Aggressor   aggressor;   // who initiated (kUnknown if unclassifiable)
  Qty         total_qty;   // summed size across legs
  std::int64_t vwap_num;   // sum(price * size); vwap = vwap_num / total_qty
  int         num_legs;
  std::size_t first_row;   // index of the first leg in the MessageTape
  bool        hidden;      // event consists of type-5 rows

  double vwap_ticks() const {
    return static_cast<double>(vwap_num) / static_cast<double>(total_qty);
  }
};

struct TradeEventOptions {
  // Legs whose timestamp is within this many ns of the event's FIRST leg are
  // eligible for grouping (anchored to the first leg, not chained, so a slow
  // drip of trades cannot daisy-chain into one giant event). Default 0 =
  // exact-ns equality, which is what LOBSTER sweeps produce.
  Nanos group_tolerance = 0;
};

// Scan the tape and produce aggregated trade events, in tape order.
// Grouping key: (timestamp within tolerance, aggressor side, hidden flag),
// over CONSECUTIVE execution rows only — any interleaved non-execution
// message breaks the group, since the book state changed mid-stream.
//
// Note: two independent same-side market orders arriving in the same
// nanosecond are indistinguishable from one sweep in this data and will be
// merged. This is the standard limitation of timestamp-based grouping.
std::vector<TradeEvent> build_trade_events(const MessageTape& messages,
                                           const BookTape& book,
                                           const TradeEventOptions& opts = {});

}  // namespace oee
