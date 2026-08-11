#include "oee/backtest/backtester.hpp"

#include <algorithm>
#include <stdexcept>

namespace oee {

Backtester::Backtester(const LobsterDay& day) : day_(&day) {
  events_ = build_trade_events(day.messages, day.book);
  event_cum_qty_.reserve(events_.size() + 1);
  event_cum_qty_.push_back(0);
  for (const TradeEvent& ev : events_) {
    event_cum_qty_.push_back(event_cum_qty_.back() + ev.total_qty);
  }
}

Qty Backtester::market_volume_between(Nanos t0, Nanos t1) const {
  // Events are in tape order, hence time-sorted. cum[i] = volume of the
  // first i events; volume in [t0, t1) is cum(first ts >= t1) - cum(first
  // ts >= t0).
  const auto lo = std::lower_bound(
      events_.begin(), events_.end(), t0,
      [](const TradeEvent& ev, Nanos t) { return ev.ts < t; });
  const auto hi = std::lower_bound(
      events_.begin(), events_.end(), t1,
      [](const TradeEvent& ev, Nanos t) { return ev.ts < t; });
  return event_cum_qty_[static_cast<std::size_t>(hi - events_.begin())] -
         event_cum_qty_[static_cast<std::size_t>(lo - events_.begin())];
}

BacktestResult Backtester::run(IExecutionStrategy& strategy,
                               const ParentOrder& parent,
                               std::unique_ptr<IImpactModel> impact) const {
  if (parent.quantity <= 0) {
    throw std::invalid_argument("Backtester: parent quantity must be > 0");
  }
  if (parent.num_slices < 1) {
    throw std::invalid_argument("Backtester: num_slices must be >= 1");
  }
  if (parent.start >= parent.end) {
    throw std::invalid_argument("Backtester: start must precede end");
  }

  ExecutionSimulator sim(*day_, std::move(impact));

  const ShiftedBookView arrival = sim.book_at(parent.start);
  if (!arrival.has_best_ask() || !arrival.has_best_bid()) {
    throw std::runtime_error(
        "Backtester: one-sided book at arrival; cannot mark M0");
  }

  BacktestResult res;
  res.strategy = strategy.name();
  res.parent = parent;
  res.arrival_mid_ticks = static_cast<double>(arrival.mid2x()) / 2.0;

  const double s = parent.side == Side::kSell ? 1.0 : -1.0;
  const Nanos window = parent.end - parent.start;

  strategy.on_start(parent);

  Qty remaining = parent.quantity;
  for (int k = 0; k < parent.num_slices && remaining > 0; ++k) {
    const Nanos t_k = parent.start + window * k / parent.num_slices;
    const ShiftedBookView book = sim.book_at(t_k);
    const ExecutionState state{
        .slice_idx = k,
        .remaining = remaining,
        .market_volume = market_volume_between(parent.start, t_k)};

    const Qty child = strategy.on_slice(t_k, book, state);
    if (child < 0 || child > remaining) {
      throw std::logic_error("Backtester: strategy '" + res.strategy +
                             "' returned invalid child size");
    }

    SliceRecord rec;
    rec.slice_idx = k;
    rec.ts = t_k;
    rec.child = child;
    rec.shift = book.shift();
    rec.mid2x_unshifted =
        day_->book.mid2x(book.row());  // raw, for drift attribution

    if (child > 0) {
      const Fill fill = sim.execute_market(t_k, parent.side, child);
      remaining -= fill.filled;
      strategy.on_fill(fill);
      rec.fill = fill;

      if (fill.filled > 0) {
        const double q = static_cast<double>(fill.filled);
        const double v = fill.vwap_ticks();
        // One-sided book at fill time => no meaningful mid; attribute zero
        // execution cost rather than a nonsense one (same guard as m_u).
        const double m_s =
            fill.mid2x_before != 0
                ? static_cast<double>(fill.mid2x_before) / 2.0
                : v;
        // Unshifted mid needs both raw sides; on a one-sided raw book we
        // conservatively attribute nothing to drift for this fill (never
        // seen in the LOBSTER samples; guarded for synthetic edge cases).
        const bool have_raw_mid = day_->book.has_best_ask(book.row()) &&
                                  day_->book.has_best_bid(book.row());
        const double m_u = have_raw_mid
                               ? static_cast<double>(rec.mid2x_unshifted) / 2.0
                               : res.arrival_mid_ticks;

        res.notional_ticks += static_cast<double>(fill.notional);
        res.cost_drift += s * q * (res.arrival_mid_ticks - m_u);
        res.cost_permanent += -s * q * static_cast<double>(rec.shift);
        res.cost_execution += s * q * (m_s - v);
      }
    }
    res.slices.push_back(rec);
  }

  res.executed = parent.quantity - remaining;
  res.remainder = remaining;

  // Terminal mark: the SHIFTED mid at parent.end — our permanent impact is
  // real in the simulated world, so leftover shares are worth the impacted
  // price, not the pristine one.
  const ShiftedBookView terminal = sim.book_at(parent.end);
  res.terminal_mid_ticks = static_cast<double>(terminal.mid2x()) / 2.0;

  res.cost_opportunity = s * static_cast<double>(res.remainder) *
                         (res.arrival_mid_ticks - res.terminal_mid_ticks);

  // Total IS from first principles (paper portfolio vs real), not by
  // summing the components — tests assert the two agree.
  const double x = static_cast<double>(parent.quantity);
  res.is_ticks_shares =
      s * (x * res.arrival_mid_ticks - res.notional_ticks -
           static_cast<double>(res.remainder) * res.terminal_mid_ticks);
  res.is_bps = res.is_ticks_shares / (x * res.arrival_mid_ticks) * 1e4;

  const Qty mkt_vol = market_volume_between(parent.start, parent.end);
  res.participation =
      mkt_vol > 0 ? static_cast<double>(res.executed) / static_cast<double>(mkt_vol)
                  : 0.0;
  return res;
}

}  // namespace oee
