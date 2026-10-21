import { marketVolume, rawMid2x } from './data';
import { PermanentImpact, executeSell } from './sim';
import type { Strategy } from './strategies';
import type { DaySeries, ParentOrder } from './types';

/** Everything the UI needs to know about one child order. */
export interface SliceRecord {
  k: number;
  /** Seconds from the start of the parent order. */
  tSec: number;
  /** Absolute grid second the child executed on. */
  gridSec: number;
  child: number;
  filled: number;
  /** Fill VWAP, ticks (0 if nothing filled). */
  vwapTicks: number;
  worstPx: number;
  levelsTouched: number;
  /** Ladder shift (ticks) in force when the child fired. */
  shift: number;
  /** 2 x shifted mid before the fill (0 if one-sided). */
  mid2xBefore: number;
  /** 2 x raw historical mid at this second. */
  rawMid2x: number;
  /** This child's cost components, in bps of the parent's arrival notional. */
  driftBps: number;
  permanentBps: number;
  executionBps: number;
  /** Shares still unexecuted after this child. */
  remainingAfter: number;
}

export interface BacktestResult {
  name: string;
  parent: ParentOrder;
  arrivalMid: number; // ticks
  terminalMid: number; // ticks, SHIFTED: our own impact is real in the simulated world
  /** Ladder shift (ticks) once every fill is done. */
  finalShift: number;
  executed: number;
  remainder: number;
  /** Total implementation shortfall, bps of arrival notional. */
  isBps: number;
  /** Decomposition; the four terms sum to isBps (asserted in tests). */
  driftBps: number;
  permanentBps: number;
  executionBps: number;
  opportunityBps: number;
  participation: number;
  avgFillTicks: number;
  slices: SliceRecord[];
}

/**
 * Perold implementation shortfall for one SELL program replayed on the real book.
 * Port of Backtester::run (cpp/src/backtest/backtester.cpp). Decomposition, with
 * M0 the arrival mid, m_u the raw mid at the fill, m_s the shifted mid, v the
 * fill VWAP and S_T the terminal shifted mid:
 *
 *   drift       = sum  q (M0 - m_u)    exogenous price movement
 *   permanent   = sum -q shift         our own accumulated impact
 *   execution   = sum  q (m_s - v)     spread + depth walked (temporary)
 *   opportunity =      R (M0 - S_T)    cost of shares left unexecuted
 */
export function runBacktest(
  day: DaySeries,
  parent: ParentOrder,
  strategy: Strategy,
  gamma: number,
): BacktestResult {
  const { qty, startSec, durationSec, slices: nSlices } = parent;
  const impact = new PermanentImpact(gamma);

  const arrivalRaw = rawMid2x(day, startSec);
  if (arrivalRaw === 0) throw new Error('one-sided book at arrival; cannot mark M0');
  const M0 = arrivalRaw / 2;
  const notional = qty * M0;
  const toBps = (ticksShares: number): number => (ticksShares / notional) * 1e4;

  let remaining = qty;
  let proceeds = 0;
  let drift = 0;
  let permanent = 0;
  let execution = 0;
  const records: SliceRecord[] = [];

  for (let k = 0; k < nSlices && remaining > 0; k++) {
    // Slice times are whole seconds because durationSec % slices === 0 (enforced by callers).
    const tSec = (durationSec * k) / nSlices;
    const gridSec = startSec + tSec;
    const shift = impact.shift();
    const marketVol = marketVolume(day, startSec, gridSec);

    const child = strategy.onSlice({ sliceIdx: k, remaining, marketVolume: marketVol });
    if (child < 0 || child > remaining) {
      throw new Error(`strategy ${strategy.name} returned invalid child size ${child}`);
    }

    const raw2x = rawMid2x(day, gridSec);
    const rec: SliceRecord = {
      k,
      tSec,
      gridSec,
      child,
      filled: 0,
      vwapTicks: 0,
      worstPx: 0,
      levelsTouched: 0,
      shift,
      mid2xBefore: 0,
      rawMid2x: raw2x,
      driftBps: 0,
      permanentBps: 0,
      executionBps: 0,
      remainingAfter: remaining,
    };

    if (child > 0) {
      const fill = executeSell(day, gridSec, child, shift);
      remaining -= fill.filled;
      impact.onFill(-fill.filled);
      rec.filled = fill.filled;
      rec.worstPx = fill.worstPx;
      rec.levelsTouched = fill.levelsTouched;
      rec.mid2xBefore = fill.mid2xBefore;
      rec.remainingAfter = remaining;

      if (fill.filled > 0) {
        const q = fill.filled;
        const v = fill.notional / q;
        rec.vwapTicks = v;
        // One-sided book at fill time: attribute zero execution cost, not a nonsense one.
        const mShifted = fill.mid2xBefore !== 0 ? fill.mid2xBefore / 2 : v;
        const mRaw = raw2x !== 0 ? raw2x / 2 : M0;

        proceeds += fill.notional;
        const d = q * (M0 - mRaw);
        const p = -q * shift;
        const e = q * (mShifted - v);
        drift += d;
        permanent += p;
        execution += e;
        rec.driftBps = toBps(d);
        rec.permanentBps = toBps(p);
        rec.executionBps = toBps(e);
      }
    }
    records.push(rec);
  }

  const executed = qty - remaining;
  const endSec = startSec + durationSec;
  const termRaw = rawMid2x(day, endSec);
  // Shifted mid at the end: raw mid + shift (both sides shift by the same ticks).
  const terminalMid = termRaw / 2 + impact.shift();
  const opportunity = remaining * (M0 - terminalMid);
  const isTicks = qty * M0 - proceeds - remaining * terminalMid;

  const mktVol = marketVolume(day, startSec, endSec);
  return {
    name: strategy.name,
    parent,
    arrivalMid: M0,
    terminalMid,
    finalShift: impact.shift(),
    executed,
    remainder: remaining,
    isBps: toBps(isTicks),
    driftBps: toBps(drift),
    permanentBps: toBps(permanent),
    executionBps: toBps(execution),
    opportunityBps: toBps(opportunity),
    participation: mktVol > 0 ? executed / mktVol : 0,
    avgFillTicks: executed > 0 ? proceeds / executed : 0,
    slices: records,
  };
}

/** sqrt(sum of squared holdings): the timing-risk exposure, proportional to Std[IS]. */
export function exposure(result: BacktestResult): number {
  // x_k for k = 1..N-1 is the inventory left after slices 0..N-2.
  let sq = 0;
  for (let j = 0; j < result.slices.length - 1; j++) sq += result.slices[j]!.remainingAfter ** 2;
  return Math.sqrt(sq);
}
