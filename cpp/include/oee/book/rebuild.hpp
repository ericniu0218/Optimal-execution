#pragma once

#include <cstddef>
#include <map>
#include <utility>
#include <vector>

#include "oee/core/types.hpp"
#include "oee/data/message_tape.hpp"

namespace oee {

// ---------------------------------------------------------------------------
// Message-driven order book reconstruction.
//
// Rebuilds the visible book from the LOBSTER message stream alone and
// validates it row-for-row against LOBSTER's own orderbook snapshots. The
// backtest does not need this (snapshot replay is correct by construction);
// it exists because reproducing the exchange's book from the raw event
// stream is the strongest end-to-end test of our understanding of the feed
// semantics — and it is the closest thing in this project to a matching
// engine.
//
// Reconstruction is LEVEL-aggregate, not order-by-order: every LOBSTER
// message carries the affected order's price and size delta explicitly
// (type 1 adds; types 2/3/4 remove; 5/6/7 leave the visible book alone),
// so per-level accumulation suffices to reproduce the displayed book, and
// it handles pre-file orders (cancellations of orders placed before the
// file window) with no order-ID bookkeeping.
//
// One thing message-only reconstruction CANNOT do, by file format: a
// level-N file omits events outside the top N levels. That cuts both
// ways. (1) When a level clears and deeper liquidity scrolls into view,
// that liquidity was never announced. (2) When the range TIGHTENS (better
// quotes arrive), previously-visible levels scroll out — and any events
// on them while outside the window are omitted, so a tracked level that
// scrolls out goes stale unobservably. The rebuild therefore PRUNES
// levels that leave the visible window (their return is handled as
// surfacing); keeping them produces false "phantom" hits that are really
// format-inherent blindness, which is exactly what debugging this against
// real data revealed. The validator classifies each row:
//
//   exact     top-K levels identical to the snapshot
//   surfaced  snapshot shows liquidity we could not have seen (a level we
//             lack, or size above ours at a price we track) -> reseed from
//             the snapshot and count; benign, format-inherent
//   hard      we claim liquidity the snapshot denies (phantom level, or
//             size above the snapshot's) -> a genuine reconstruction bug
//
// The acceptance bar is ZERO hard rows across a full trading day, with
// surfaced rows counted and reported, not hidden.
// ---------------------------------------------------------------------------

struct RebuildStats {
  std::size_t rows = 0;            // rows validated (row 0 seeds the book)
  std::size_t exact_rows = 0;
  std::size_t surfaced_rows = 0;   // rows needing >= 1 benign reseed
  std::size_t hard_rows = 0;       // rows with a genuine mismatch
  std::size_t surfaced_levels = 0; // total reseeded levels
  std::size_t pruned_levels = 0;   // levels dropped on leaving the window
  std::size_t clamped_deltas = 0;  // reductions clamped at zero
  std::ptrdiff_t first_hard_row = -1;

  bool clean() const { return hard_rows == 0; }
};

// Price-level book, one aggregate per occupied price.
class RebuiltBook {
 public:
  using Level = std::pair<PriceTicks, Qty>;

  void add(std::int8_t dir, PriceTicks px, Qty qty);
  // Returns the amount that could not be removed (clamped at zero).
  Qty reduce(std::int8_t dir, PriceTicks px, Qty qty);
  void set_level(std::int8_t dir, PriceTicks px, Qty qty);

  // Best-first levels of one side, at most k.
  std::vector<Level> top(std::int8_t dir, int k) const;

  // Replace one side wholesale from snapshot levels (post-hard resync so a
  // single fault cannot cascade through the rest of the day).
  void resync(std::int8_t dir, const std::vector<Level>& levels);

  // Drop all levels strictly worse than `worst_visible` (further from the
  // touch). Returns how many were dropped. Called when the snapshot is
  // truncated at the requested depth: anything beyond its worst shown
  // price is leaving the observable window and will go stale.
  std::size_t prune_beyond(std::int8_t dir, PriceTicks worst_visible);

 private:
  std::map<PriceTicks, Qty, std::greater<PriceTicks>> bids_;
  std::map<PriceTicks, Qty> asks_;
};

// Rebuild the day from messages, validating the top `check_levels` levels
// (0 = all levels present in the book tape) against every snapshot row.
RebuildStats validate_rebuild(const LobsterDay& day, int check_levels = 0);

}  // namespace oee
