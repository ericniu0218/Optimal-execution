#pragma once

#include <string>

#include "oee/core/types.hpp"
#include "oee/market/execution_simulator.hpp"

namespace oee {

// ---------------------------------------------------------------------------
// Strategy interface.
//
// A strategy is pure decision logic: at each slice boundary it is shown the
// (impact-shifted) market and its own execution state, and returns the child
// quantity to send NOW as a marketable order. The backtester owns the clock,
// the simulator, and the accounting — so strategies are unit-testable with
// hand-built states and no market machinery.
//
// Slicing convention: the parent window [start, end) is divided into
// num_slices equal intervals; on_slice is called at each interval's start
// (slice_idx = 0..num_slices-1). Strategies fire at child-order frequency
// (seconds), so virtual dispatch here costs nothing that matters; the hot
// paths (parsing, fills) are non-virtual by design.
// ---------------------------------------------------------------------------

struct ParentOrder {
  Side side = Side::kSell;   // sell-side liquidation is canonical
  Qty quantity = 0;          // total shares to execute
  Nanos start = 0;           // first slice fires here
  Nanos end = 0;             // all trading done before this
  int num_slices = 0;
};

// What the strategy is allowed to know about its own progress and the tape.
struct ExecutionState {
  int slice_idx = 0;         // current slice, 0-based
  Qty remaining = 0;         // parent quantity still unexecuted
  // Market trade volume (visible + hidden trade events, all participants)
  // since parent start, NOT including our own simulated fills — our fills
  // are not part of the replayed tape. This is what a POV strategy paces on.
  Qty market_volume = 0;
};

class IExecutionStrategy {
 public:
  virtual ~IExecutionStrategy() = default;

  virtual std::string name() const = 0;

  // Called once before the first slice. Strategies validate parent/config
  // compatibility here (throw std::invalid_argument on misconfiguration).
  virtual void on_start(const ParentOrder& parent) = 0;

  // Return the child quantity (>= 0, <= state.remaining) to send at this
  // slice as a marketable order. Returning 0 skips the slice.
  virtual Qty on_slice(Nanos now, const ShiftedBookView& book,
                       const ExecutionState& state) = 0;

  // Notification of what actually happened (fills may be partial).
  virtual void on_fill(const Fill& fill) { (void)fill; }
};

}  // namespace oee
