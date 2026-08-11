#pragma once

#include <string>

#include "oee/strategy/strategy.hpp"

namespace oee {

// ---------------------------------------------------------------------------
// Percentage-of-volume strategy.
//
// Targets executing `participation` fraction of observed market volume:
// at each slice it sends participation * (market volume in the PREVIOUS
// interval) — trailing volume as the predictor of current volume, which is
// how a simple POV engine actually paces (it cannot see the future volume
// of the interval it is entering). The first slice therefore sends nothing.
//
// A pure POV policy does not guarantee completion inside a fixed window —
// its natural completion time is endogenous to market volume. Since the
// backtest compares strategies on the same [start, end) window, the default
// forces any remainder into the final slice (documented distortion, flag to
// disable). With cleanup off, the backtester values unexecuted shares at
// the terminal mid, Perold-style.
// ---------------------------------------------------------------------------
class PovStrategy final : public IExecutionStrategy {
 public:
  explicit PovStrategy(double participation, bool cleanup_final_slice = true);

  std::string name() const override;
  void on_start(const ParentOrder& parent) override;
  Qty on_slice(Nanos now, const ShiftedBookView& book,
               const ExecutionState& state) override;

 private:
  double participation_;
  bool cleanup_final_slice_;
  ParentOrder parent_{};
  Qty prev_market_volume_ = 0;
};

}  // namespace oee
