#include "oee/market/impact_model.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace oee {
namespace {

constexpr Nanos kT0 = 34200 * kNanosPerSec;
Nanos at_sec(double s) { return kT0 + static_cast<Nanos>(s * 1e9); }

TEST(TransientImpact, PermanentComponentNeverDecays) {
  TransientImpact m(/*gamma=*/0.01, /*kappa=*/0.0, /*rho=*/1.0);
  m.on_fill(kT0, -1000);  // sold 1000
  EXPECT_EQ(m.ladder_shift(kT0), -10);
  EXPECT_EQ(m.ladder_shift(at_sec(3600)), -10);  // an hour later: unchanged
}

TEST(TransientImpact, TransientComponentDecaysExponentially) {
  // kappa = 0.01 t/sh, 1000 shares sold => -10 ticks at t=0.
  // rho = ln2 => half-life exactly 1 second.
  const double rho = std::log(2.0);
  TransientImpact m(/*gamma=*/0.0, /*kappa=*/0.01, rho);
  m.on_fill(kT0, -1000);

  EXPECT_NEAR(m.half_life_sec(), 1.0, 1e-12);
  EXPECT_EQ(m.ladder_shift(kT0), -10);
  EXPECT_EQ(m.ladder_shift(at_sec(1)), -5);   // one half-life
  EXPECT_EQ(m.ladder_shift(at_sec(2)), -3);   // llround(-2.5) -> -3
  EXPECT_EQ(m.ladder_shift(at_sec(20)), 0);   // fully healed
}

TEST(TransientImpact, RhoZeroMakesTransientPermanent) {
  // The rho -> 0 limit: the transient term merges into the permanent one,
  // so kappa and gamma become interchangeable.
  TransientImpact transient_only(/*gamma=*/0.0, /*kappa=*/0.01, /*rho=*/0.0);
  TransientImpact permanent_only(/*gamma=*/0.01, /*kappa=*/0.0, /*rho=*/0.0);
  transient_only.on_fill(kT0, -1000);
  permanent_only.on_fill(kT0, -1000);
  for (const double s : {0.0, 1.0, 60.0, 3600.0}) {
    EXPECT_EQ(transient_only.ladder_shift(at_sec(s)),
              permanent_only.ladder_shift(at_sec(s)));
  }
}

TEST(TransientImpact, LargeRhoReproducesPass1PermanentModel) {
  // The rho -> infinity limit: the book heals instantly between child
  // orders, which is exactly what Pass 1 assumed. With a 10ms half-life,
  // one-second-spaced children see only the permanent component.
  TransientImpact fast_heal(/*gamma=*/0.01, /*kappa=*/0.05, /*rho=*/700.0);
  LinearPermanentImpact pass1(0.01);
  for (int k = 0; k < 5; ++k) {
    fast_heal.on_fill(at_sec(k), -1000);
    pass1.on_fill(at_sec(k), -1000);
  }
  EXPECT_EQ(fast_heal.ladder_shift(at_sec(10)), pass1.ladder_shift(at_sec(10)));
}

TEST(TransientImpact, ContributionsSuperposeWithTheirOwnClocks) {
  // Two fills a half-life apart: the older has decayed to half, the newer
  // is fresh. Total at t=1s: 0.01*(1000*0.5 + 1000*1.0) = 15 ticks.
  const double rho = std::log(2.0);
  TransientImpact m(/*gamma=*/0.0, /*kappa=*/0.01, rho);
  m.on_fill(kT0, -1000);
  m.on_fill(at_sec(1), -1000);
  EXPECT_EQ(m.ladder_shift(at_sec(1)), -15);
  // One further half-life: both halve again.
  EXPECT_EQ(m.ladder_shift(at_sec(2)), -8);  // llround(-7.5) -> -8
}

TEST(TransientImpact, BuysAndSellsOffset) {
  const double rho = std::log(2.0);
  TransientImpact m(/*gamma=*/0.01, /*kappa=*/0.01, rho);
  m.on_fill(kT0, -1000);
  m.on_fill(kT0, +1000);  // immediately bought it back
  EXPECT_EQ(m.ladder_shift(kT0), 0);
  EXPECT_EQ(m.ladder_shift(at_sec(60)), 0);
}

TEST(TransientImpact, BackwardsQueryDoesNotAmplify) {
  // Defensive: a mis-ordered query must never return MORE impact than the
  // state holds (exp(-rho*negative) > 1 would inflate it).
  const double rho = std::log(2.0);
  TransientImpact m(/*gamma=*/0.0, /*kappa=*/0.01, rho);
  m.on_fill(at_sec(10), -1000);
  EXPECT_EQ(m.ladder_shift(at_sec(5)), -10);  // clamped, not -20
}

TEST(TransientImpact, RoundsCumulativeStateNotPerFill) {
  // kappa = 0.004: each 100-share fill is 0.4 ticks. Per-fill rounding
  // would floor all of them to zero; cumulative rounding gives 1 tick.
  TransientImpact m(/*gamma=*/0.0, /*kappa=*/0.004, /*rho=*/0.0);
  for (int k = 0; k < 3; ++k) m.on_fill(kT0, -100);
  EXPECT_EQ(m.ladder_shift(kT0), -1);
}

}  // namespace
}  // namespace oee
