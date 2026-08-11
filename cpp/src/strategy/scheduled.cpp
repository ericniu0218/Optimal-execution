#include "oee/strategy/scheduled.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace oee {

std::vector<double> twap_weights(int num_slices) {
  if (num_slices < 1) {
    throw std::invalid_argument("twap_weights: num_slices must be >= 1");
  }
  std::vector<double> w(static_cast<std::size_t>(num_slices) + 1);
  for (int j = 0; j <= num_slices; ++j) {
    w[static_cast<std::size_t>(j)] =
        static_cast<double>(num_slices - j) / num_slices;
  }
  return w;
}

std::vector<double> vwap_weights(int num_slices, double smile) {
  if (num_slices < 1) {
    throw std::invalid_argument("vwap_weights: num_slices must be >= 1");
  }
  if (smile < 0.0) {
    throw std::invalid_argument("vwap_weights: smile must be >= 0");
  }
  // Cumulative of v(u) = 1 + smile*(2u-1)^2:
  //   V(u) = u + smile*((2u-1)^3 + 1)/6,  normalized by V(1) = 1 + smile/3.
  const auto cum = [smile](double u) {
    const double m = 2.0 * u - 1.0;
    return (u + smile * (m * m * m + 1.0) / 6.0) / (1.0 + smile / 3.0);
  };
  std::vector<double> w(static_cast<std::size_t>(num_slices) + 1);
  for (int j = 0; j <= num_slices; ++j) {
    w[static_cast<std::size_t>(j)] =
        1.0 - cum(static_cast<double>(j) / num_slices);
  }
  w.front() = 1.0;  // pin against rounding
  w.back() = 0.0;
  return w;
}

std::vector<double> ac_weights(int num_slices, double horizon_T,
                               const ac::Params& params,
                               bool continuous_kappa) {
  // Solving with X = 1 makes the holdings vector its own weight schedule.
  return ac::solve(1.0, horizon_T, num_slices, params, continuous_kappa)
      .holdings;
}

ScheduledStrategy::ScheduledStrategy(std::string name,
                                     std::vector<double> weights)
    : name_(std::move(name)), weights_(std::move(weights)) {
  if (weights_.size() < 2) {
    throw std::invalid_argument("ScheduledStrategy: need at least w_0, w_N");
  }
  if (weights_.front() != 1.0 || weights_.back() != 0.0) {
    throw std::invalid_argument(
        "ScheduledStrategy: weights must run from 1.0 to 0.0");
  }
  for (std::size_t j = 1; j < weights_.size(); ++j) {
    if (weights_[j] > weights_[j - 1]) {
      throw std::invalid_argument(
          "ScheduledStrategy: weights must be non-increasing");
    }
  }
}

void ScheduledStrategy::on_start(const ParentOrder& parent) {
  if (parent.num_slices != static_cast<int>(weights_.size()) - 1) {
    throw std::invalid_argument(
        "ScheduledStrategy: parent.num_slices does not match schedule length");
  }
  if (parent.quantity <= 0) {
    throw std::invalid_argument("ScheduledStrategy: parent quantity <= 0");
  }
  parent_ = parent;
}

Qty ScheduledStrategy::on_slice(Nanos /*now*/, const ShiftedBookView& /*book*/,
                                const ExecutionState& state) {
  // Planned cumulative execution by the END of this slice.
  const double target_frac =
      1.0 - weights_[static_cast<std::size_t>(state.slice_idx) + 1];
  const Qty planned_cum = static_cast<Qty>(
      std::llround(static_cast<double>(parent_.quantity) * target_frac));
  const Qty executed = parent_.quantity - state.remaining;
  // Catch-up sizing: shortfall from partial fills or rounding in earlier
  // slices lands here instead of being lost.
  return std::clamp<Qty>(planned_cum - executed, 0, state.remaining);
}

}  // namespace oee
