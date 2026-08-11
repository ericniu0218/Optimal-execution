#include "oee/strategy/adaptive_ac.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace oee {

AdaptiveAcStrategy::AdaptiveAcStrategy(ac::Params base, double horizon,
                                       double cap_frac, int min_obs)
    : base_(base), horizon_(horizon), cap_frac_(cap_frac), min_obs_(min_obs) {
  if (!(horizon > 0.0)) {
    throw std::invalid_argument("AdaptiveAcStrategy: horizon must be > 0");
  }
  if (cap_frac < 0.0 || cap_frac > 1.0) {
    throw std::invalid_argument("AdaptiveAcStrategy: cap_frac must be in [0,1]");
  }
  base_.alpha = 0.0;  // the estimate replaces whatever was passed
  char buf[48];
  std::snprintf(buf, sizeof(buf), "AdaptAC(%.0e|c%.2g)", base.lambda, cap_frac);
  name_ = buf;
}

void AdaptiveAcStrategy::on_start(const ParentOrder& parent) {
  if (parent.quantity <= 0) {
    throw std::invalid_argument("AdaptiveAcStrategy: parent quantity <= 0");
  }
  if (parent.num_slices < 1) {
    throw std::invalid_argument("AdaptiveAcStrategy: num_slices must be >= 1");
  }
  parent_ = parent;
  obs_t_.clear();
  obs_mid_.clear();
  last_alpha_ = 0.0;
}

double AdaptiveAcStrategy::estimate_drift() const {
  const std::size_t n = obs_t_.size();
  double mt = 0.0, mm = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    mt += obs_t_[i];
    mm += obs_mid_[i];
  }
  mt /= static_cast<double>(n);
  mm /= static_cast<double>(n);
  double stt = 0.0, stm = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double dt = obs_t_[i] - mt;
    stt += dt * dt;
    stm += dt * (obs_mid_[i] - mm);
  }
  return stt > 0.0 ? stm / stt : 0.0;
}

Qty AdaptiveAcStrategy::on_slice(Nanos now, const ShiftedBookView& book,
                                 const ExecutionState& state) {
  // Observation clock in the solver's time unit (minutes), so the estimated
  // slope is directly comparable to sigma (ticks per sqrt(minute)).
  const double elapsed = static_cast<double>(now - parent_.start) /
                         static_cast<double>(kNanosPerSec) / 60.0;

  if (book.has_best_ask() && book.has_best_bid()) {
    obs_t_.push_back(elapsed);
    obs_mid_.push_back(static_cast<double>(book.mid2x()) / 2.0);
  }

  const int slices_left = parent_.num_slices - state.slice_idx;
  if (slices_left <= 1) return state.remaining;  // final slice: finish

  // Drift estimate, shrunk. For a BUY program the sign flips: a rising
  // market makes waiting expensive rather than profitable.
  double alpha = 0.0;
  if (static_cast<int>(obs_t_.size()) >= min_obs_ && cap_frac_ > 0.0) {
    alpha = estimate_drift();
    if (parent_.side == Side::kBuy) alpha = -alpha;
    // Clamp so the drift-implied holding xbar = alpha/(2 lambda sigma^2)
    // never exceeds cap_frac of what is left to trade. Without this the
    // schedule is a leveraged momentum bet on a noisy slope estimate.
    const double denom = 2.0 * base_.lambda * base_.sigma * base_.sigma;
    if (denom > 0.0) {
      const double alpha_max =
          cap_frac_ * static_cast<double>(state.remaining) * denom;
      alpha = std::clamp(alpha, -alpha_max, alpha_max);
    }
  }
  last_alpha_ = alpha;

  const double time_left = horizon_ * static_cast<double>(slices_left) /
                           static_cast<double>(parent_.num_slices);

  ac::Params p = base_;
  p.alpha = alpha;
  try {
    const ac::Solution sol = ac::solve(static_cast<double>(state.remaining),
                                       time_left, slices_left, p);
    // Drift can push the unconstrained optimum outside [0, remaining]
    // (a large positive alpha wants to BUY inventory back); this is a
    // liquidation program, so clamp rather than reverse.
    return std::clamp<Qty>(static_cast<Qty>(std::llround(sol.trades.front())),
                           0, state.remaining);
  } catch (const std::exception&) {
    // Ill-posed at this remaining horizon (eta~ <= 0 as tau grows): fall
    // back to the TWAP-equivalent slice rather than dropping the schedule.
    return std::clamp<Qty>(state.remaining / slices_left, 0, state.remaining);
  }
}

}  // namespace oee
