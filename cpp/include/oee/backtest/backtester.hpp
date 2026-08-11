#pragma once

#include <memory>
#include <string>
#include <vector>

#include "oee/core/types.hpp"
#include "oee/data/message_tape.hpp"
#include "oee/data/trade_events.hpp"
#include "oee/market/execution_simulator.hpp"
#include "oee/strategy/strategy.hpp"

namespace oee {

// ---------------------------------------------------------------------------
// Backtester: runs one strategy over one parent order against one replayed
// day, and accounts the result as Perold implementation shortfall with a
// full decomposition. Every strategy in a comparison runs through this exact
// loop — same day, same parent, same slice grid, same fill mechanics — so
// differences in IS are attributable to the schedule alone.
//
// Cost decomposition (all in ticks*shares, positive = cost, s = +1 sell /
// -1 buy, M0 = arrival mid, m_u = unshifted mid at fill, m_s = shifted mid
// at fill, v = fill VWAP, S_T = terminal shifted mid, R = unexecuted):
//
//   drift        = sum s*q*(M0 - m_u)     exogenous price movement
//   permanent    = sum -s*q*shift         our own accumulated impact
//   execution    = sum s*q*(m_s - v)      spread + depth walked (temporary)
//   opportunity  = s*R*(M0 - S_T)         Perold cost of not completing
//
// These sum to IS = s*(X*M0 - proceeds - R*S_T) exactly, by construction —
// the identity is asserted in tests, not assumed.
// ---------------------------------------------------------------------------

struct SliceRecord {
  int slice_idx = 0;
  Nanos ts = 0;
  Qty child = 0;       // what the strategy asked for
  Fill fill;           // what happened (fill.filled == 0 if child was 0)
  PriceTicks shift = 0;          // ladder shift when the slice fired
  PriceTicks mid2x_unshifted = 0;  // raw historical 2x mid at the slice row
};

struct BacktestResult {
  std::string strategy;
  ParentOrder parent;

  double arrival_mid_ticks = 0.0;   // M0
  double terminal_mid_ticks = 0.0;  // S_T (shifted; our impact is real here)
  Qty executed = 0;
  Qty remainder = 0;
  double notional_ticks = 0.0;      // proceeds (sell) / outlay (buy)

  double is_ticks_shares = 0.0;     // total implementation shortfall
  double is_bps = 0.0;              // vs arrival notional X*M0

  double cost_drift = 0.0;
  double cost_permanent = 0.0;
  double cost_execution = 0.0;
  double cost_opportunity = 0.0;

  // executed / total market trade volume in [start, end)
  double participation = 0.0;

  std::vector<SliceRecord> slices;

  double avg_fill_ticks() const {
    return executed > 0 ? notional_ticks / static_cast<double>(executed) : 0.0;
  }
};

class Backtester {
 public:
  // Aggregates trade events once (market-volume feed for POV/participation);
  // the day must outlive the backtester.
  explicit Backtester(const LobsterDay& day);

  // Run one strategy. A fresh simulator (fresh impact state) per run, so
  // runs are independent and order does not matter. The impact model is
  // per-run because it is stateful.
  BacktestResult run(IExecutionStrategy& strategy, const ParentOrder& parent,
                     std::unique_ptr<IImpactModel> impact) const;

  // Market trade volume (visible + hidden, all participants) in [t0, t1).
  Qty market_volume_between(Nanos t0, Nanos t1) const;

 private:
  const LobsterDay* day_;
  std::vector<TradeEvent> events_;
  std::vector<Qty> event_cum_qty_;  // prefix sums aligned with events_
};

}  // namespace oee
