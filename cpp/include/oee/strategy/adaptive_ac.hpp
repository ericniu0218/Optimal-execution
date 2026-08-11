#pragma once

#include <string>
#include <vector>

#include "oee/ac/solver.hpp"
#include "oee/strategy/strategy.hpp"

namespace oee {

// ---------------------------------------------------------------------------
// Adaptive Almgren-Chriss.
//
// WHY THE OBVIOUS VERSION IS VACUOUS. The natural "adaptive AC" — re-solve
// the same problem each slice on the remaining shares and remaining time —
// adds nothing. Under mean-variance the re-solve is time-inconsistent (the
// continuation of an optimal plan is not the plan a fresh optimizer picks),
// and under CARA utility with Gaussian prices the optimal strategy is
// provably DETERMINISTIC (Schied & Schoneborn 2009): no realized price path
// should change the schedule. Re-solving the drift-free problem therefore
// reproduces the static trajectory while looking adaptive.
//
// WHAT MAKES IT REAL. Adaptivity has to enter through information the static
// solve did not have. Here that is the DRIFT: alpha is re-estimated from the
// realized mid path, and the remaining trajectory is re-solved with it (the
// closed form of ac::solve with p.alpha != 0). Selling into a market that is
// drifting up, holding inventory pays, so the schedule slows; drifting down,
// it accelerates. The re-solve is justified by new information rather than
// by re-litigating a solved problem — which is the honest distinction, and
// the residual time-inconsistency of the mean-variance objective is noted
// rather than hidden.
//
// This is a momentum bet in execution clothing: it only helps if short-horizon
// drift persists over the remaining horizon. Whether it does is an empirical
// question the multi-window backtest answers, in either direction.
// ---------------------------------------------------------------------------
class AdaptiveAcStrategy final : public IExecutionStrategy {
 public:
  // `base` supplies sigma/gamma/eta/epsilon/lambda; its alpha is ignored and
  // replaced by the running estimate.
  // `horizon` is the parent duration in the solver's time unit (minutes).
  // `cap_frac` bounds how far drift may bend the schedule, and it is the
  //   load-bearing safety parameter. Drift enters the solution through the
  //   constant xbar = alpha / (2*lambda*sigma^2) — the inventory the drift
  //   alone justifies holding — and that denominator is TINY at realistic
  //   lambda, so an unbounded estimate produces xbar far larger than the
  //   whole order and defers everything to the deadline. Measured
  //   uncapped on real data: std of IS blew up from 10 bps to 342 bps.
  //   We therefore clamp alpha so |xbar| <= cap_frac * (shares remaining).
  //   0 disables adaptivity entirely (recovering static AC).
  // `min_obs` slices must be observed before any drift is acted on.
  AdaptiveAcStrategy(ac::Params base, double horizon, double cap_frac = 0.25,
                     int min_obs = 5);

  std::string name() const override { return name_; }
  void on_start(const ParentOrder& parent) override;
  Qty on_slice(Nanos now, const ShiftedBookView& book,
               const ExecutionState& state) override;

  // Most recent drift estimate, in ticks per unit time (signed for a sell).
  double last_alpha() const { return last_alpha_; }

 private:
  // OLS slope of observed mid on elapsed time, in ticks per unit time.
  double estimate_drift() const;

  ac::Params base_;
  double horizon_;
  double cap_frac_;
  int min_obs_;
  std::string name_;
  ParentOrder parent_{};
  std::vector<double> obs_t_;    // elapsed time, solver units
  std::vector<double> obs_mid_;  // mid in ticks
  double last_alpha_ = 0.0;
};

}  // namespace oee
