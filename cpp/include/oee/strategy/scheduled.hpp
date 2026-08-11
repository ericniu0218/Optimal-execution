#pragma once

#include <string>
#include <vector>

#include "oee/ac/solver.hpp"
#include "oee/strategy/strategy.hpp"

namespace oee {

// ---------------------------------------------------------------------------
// Static-schedule strategies.
//
// TWAP, VWAP, and Almgren-Chriss are all the SAME strategy: precompute a
// target-holdings curve and trade the difference at each slice. They differ
// only in the curve. This makes "AC generalizes TWAP as lambda -> 0" a
// structural fact in the code, not just a line in the write-up.
//
// A schedule is a weight vector w_0..w_N with w_0 = 1, w_N = 0,
// non-increasing: w_j is the FRACTION of the parent order still unexecuted
// at the start of slice j.
// ---------------------------------------------------------------------------

// Straight line: w_j = 1 - j/N.
std::vector<double> twap_weights(int num_slices);

// Parametric intraday U-curve. Instantaneous volume at normalized time
// u in [0,1] is modeled as v(u) = 1 + smile*(2u-1)^2 — open and close
// trade ~(1+smile)x the midday rate. The schedule follows the cumulative
// volume curve. Parametric rather than empirical by design: with a single
// free sample day, fitting the curve to the same day we backtest on would
// hand VWAP in-sample information no live desk has (disclosed in README).
std::vector<double> vwap_weights(int num_slices, double smile = 2.0);

// Almgren-Chriss optimal schedule: w = normalized ac::solve holdings.
// horizon_T is in the same time units as the ac::Params (the calibration
// layer owns unit consistency).
std::vector<double> ac_weights(int num_slices, double horizon_T,
                               const ac::Params& params,
                               bool continuous_kappa = false);

// Executes any weight schedule against a parent order. Child sizing is
// self-correcting: each slice targets the PLANNED CUMULATIVE execution
//   C_k = round(X * (1 - w_{k+1}))
// and sends C_k minus what has actually executed so far. Partial fills and
// integer rounding therefore roll into later slices instead of silently
// drifting the schedule; the final slice always targets X exactly.
class ScheduledStrategy final : public IExecutionStrategy {
 public:
  ScheduledStrategy(std::string name, std::vector<double> weights);

  std::string name() const override { return name_; }
  void on_start(const ParentOrder& parent) override;
  Qty on_slice(Nanos now, const ShiftedBookView& book,
               const ExecutionState& state) override;

 private:
  std::string name_;
  std::vector<double> weights_;
  ParentOrder parent_{};
};

}  // namespace oee
