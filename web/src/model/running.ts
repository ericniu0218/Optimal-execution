import { exposure, type BacktestResult } from '../engine';

/** The scoreboard at replay time t: every fill so far, in bps of arrival notional. */
export interface Running {
  drift: number;
  perm: number;
  exec: number;
  opp: number;
  total: number;
  soldFrac: number;
}

/**
 * Cost is additive over fills, so a running tally converges exactly to the final
 * implementation shortfall; the cost of shares left unsold lands at the end.
 */
export function runningAt(r: BacktestResult, t: number): Running {
  let drift = 0;
  let perm = 0;
  let exec = 0;
  let sold = 0;
  for (const s of r.slices) {
    if (s.tSec > t) break;
    drift += s.driftBps;
    perm += s.permanentBps;
    exec += s.executionBps;
    sold += s.filled;
  }
  const opp = t >= r.parent.durationSec ? r.opportunityBps : 0;
  return { drift, perm, exec, opp, total: drift + perm + exec + opp, soldFrac: sold / r.parent.qty };
}

/** Cost the strategy controls: impact it caused plus unsold shares, with the market's drift removed. */
export const impactOf = (r: BacktestResult): number => r.permanentBps + r.executionBps + r.opportunityBps;

/** Fraction less (positive) or more (negative) inventory risk than a reference run. */
export const riskReduction = (r: BacktestResult, ref: BacktestResult): number =>
  1 - exposure(r) / exposure(ref);
