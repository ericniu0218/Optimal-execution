#include "oee/strategy/pov.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace oee {

PovStrategy::PovStrategy(double participation, bool cleanup_final_slice)
    : participation_(participation), cleanup_final_slice_(cleanup_final_slice) {
  if (!(participation > 0.0) || !(participation < 1.0)) {
    throw std::invalid_argument("PovStrategy: participation must be in (0,1)");
  }
}

std::string PovStrategy::name() const {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "POV(%g%%)", participation_ * 100.0);
  return buf;
}

void PovStrategy::on_start(const ParentOrder& parent) {
  if (parent.quantity <= 0) {
    throw std::invalid_argument("PovStrategy: parent quantity <= 0");
  }
  parent_ = parent;
  prev_market_volume_ = 0;
}

Qty PovStrategy::on_slice(Nanos /*now*/, const ShiftedBookView& /*book*/,
                          const ExecutionState& state) {
  if (cleanup_final_slice_ && state.slice_idx == parent_.num_slices - 1) {
    return state.remaining;
  }
  const Qty interval_volume = state.market_volume - prev_market_volume_;
  prev_market_volume_ = state.market_volume;
  const Qty child = static_cast<Qty>(
      std::llround(participation_ * static_cast<double>(interval_volume)));
  return std::clamp<Qty>(child, 0, state.remaining);
}

}  // namespace oee
