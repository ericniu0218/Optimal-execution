#pragma once

#include <cmath>
#include <limits>

#include "oee/core/types.hpp"

namespace oee {

// ---------------------------------------------------------------------------
// The strategy's own market impact on the replayed book.
//
// Division of labor, deliberately:
//   - INSTANTANEOUS cost is EMERGENT in the simulator: a child order crosses
//     the spread (the epsilon term) and walks visible depth (the eta term).
//     Applying a parametric penalty for that on top would double-count it.
//     The parametric eta lives only in the planning layer (ac::solve).
//   - PERSISTENT impact is the piece replayed data cannot give us — the
//     historical tape never saw our trades — so it is modeled here and
//     applied as a uniform shift to every price level of the replayed
//     ladder for all subsequent events.
//
// Shifting the whole ladder (rather than adjusting fill prices at accounting
// time) matters because strategies observe the market mid-run: POV and
// adaptive variants must see the impacted prices, not the pristine ones.
// The approximation being made is explicit: level PRICES move, level SHAPE
// (depth) does not.  See README "the backtest is not a counterfactual".
//
// The interface takes a clock because impact decays: what our trading did
// to the market ten minutes ago is not what it did ten seconds ago.
// ---------------------------------------------------------------------------
class IImpactModel {
 public:
  virtual ~IImpactModel() = default;

  // Shift applied to every replayed price level as of `now`, in integer
  // ticks (integer so shifted prices stay on the exchange price lattice).
  // Negative after net selling.
  virtual PriceTicks ladder_shift(Nanos now) const = 0;

  // Record an executed fill. signed_qty > 0 means we bought, < 0 we sold.
  virtual void on_fill(Nanos ts, Qty signed_qty) = 0;
};

// Null model: replay with no impact feedback. Exists so "how much does the
// impact overlay change the answer" is a one-line experiment.
class NoImpact final : public IImpactModel {
 public:
  PriceTicks ladder_shift(Nanos) const override { return 0; }
  void on_fill(Nanos, Qty) override {}
};

// ---------------------------------------------------------------------------
// Transient impact with exponential resilience (Obizhaeva & Wang, 2013).
//
//   shift(t) = gamma * Q(t)  +  kappa * sum_i q_i * exp(-rho * (t - t_i))
//              \___________/     \_______________________________________/
//               permanent          transient: decays with half-life
//               (never decays)     ln2/rho as the book heals
//
// This nests everything the project used before it:
//   rho -> infinity  : the transient term vanishes instantly => the pure
//                      permanent model (what Pass 1 backtests assumed, i.e.
//                      the book fully refills between child orders)
//   rho -> 0         : the transient term never decays => it merges into
//                      the permanent coefficient
//
// Modeling this matters because the simulator walks REAL historical depth,
// which refills for free between events. Without a transient term the
// backtest implicitly assumes a book that heals instantly — generous to
// fast strategies, exactly where the AC tradeoff is being measured.
//
// The exponential state is kept in double and decayed by elapsed time on
// every touch; rounding to ticks happens once, at query. (Rounding per fill
// would floor every small child's contribution to zero.)
// ---------------------------------------------------------------------------
class TransientImpact final : public IImpactModel {
 public:
  // gamma, kappa in ticks per share; rho in 1/second (0 => never decays).
  TransientImpact(double gamma_ticks_per_share, double kappa_ticks_per_share,
                  double rho_per_sec)
      : gamma_(gamma_ticks_per_share),
        kappa_(kappa_ticks_per_share),
        rho_(rho_per_sec) {}

  PriceTicks ladder_shift(Nanos now) const override {
    return static_cast<PriceTicks>(
        std::llround(gamma_ * cum_signed_qty_ + kappa_ * decayed_to(now)));
  }

  void on_fill(Nanos ts, Qty signed_qty) override {
    transient_ = decayed_to(ts);
    last_ts_ = ts;
    cum_signed_qty_ += static_cast<double>(signed_qty);
    transient_ += static_cast<double>(signed_qty);
  }

  // Half-life of the transient component, in seconds (infinity if rho = 0).
  double half_life_sec() const {
    return rho_ > 0.0 ? std::log(2.0) / rho_
                      : std::numeric_limits<double>::infinity();
  }

 private:
  double decayed_to(Nanos now) const {
    if (transient_ == 0.0 || rho_ <= 0.0) return transient_;
    const double dt =
        static_cast<double>(now - last_ts_) / static_cast<double>(kNanosPerSec);
    // Queries never run backwards in a backtest, but a defensive clamp
    // keeps a mis-ordered call from AMPLIFYING impact.
    return dt <= 0.0 ? transient_ : transient_ * std::exp(-rho_ * dt);
  }

  double gamma_;
  double kappa_;
  double rho_;
  double cum_signed_qty_ = 0.0;
  double transient_ = 0.0;
  Nanos last_ts_ = 0;
};

// Almgren-Chriss permanent impact: shift = gamma * (net signed shares).
// The Pass 1 model, kept as a named alias because it is the AC assumption
// itself and the natural baseline to compare resilience against.
class LinearPermanentImpact final : public IImpactModel {
 public:
  explicit LinearPermanentImpact(double gamma_ticks_per_share)
      : inner_(gamma_ticks_per_share, 0.0, 0.0) {}

  PriceTicks ladder_shift(Nanos now) const override {
    return inner_.ladder_shift(now);
  }
  void on_fill(Nanos ts, Qty signed_qty) override {
    inner_.on_fill(ts, signed_qty);
  }

 private:
  TransientImpact inner_;
};

}  // namespace oee
