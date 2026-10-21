/** One replayable trading day on a 1-second grid. See scripts/build-data.py. */
export interface DaySeries {
  /** Number of one-second snapshots (grid seconds 0..n-1). */
  n: number;
  /** Book depth per side. */
  levels: number;
  /** Seconds after midnight of grid second 0 (market open). */
  openSec: number;
  /**
   * Column-major prices/sizes, delta-decoded. Layout (each column has n entries):
   *   [askPx x L][askSz x L][bidPx x L][bidSz x L][volume]
   * Prices are integer ticks (1e-4 dollars); an empty level is price 0, size 0.
   */
  cols: Int32Array;
  /** volPrefix[i] = market volume summed over grid bins [0, i). Length n + 1. */
  volPrefix: Float64Array;
}

/** A sell program. Times are whole grid seconds so book lookups are exact. */
export interface ParentOrder {
  qty: number;
  startSec: number;
  durationSec: number;
  slices: number;
}

/** Calibrated market parameters, in the planner's units (ticks, shares, minutes). */
export interface MarketParams {
  /** Permanent impact, ticks of ladder shift per share sold. */
  gamma: number;
  /** Temporary impact, ticks per (share / minute). */
  eta: number;
  /** Volatility, ticks per sqrt(minute). */
  sigma: number;
  /** Fixed cost per share (half spread), ticks. */
  epsilon: number;
}

export interface AcParams extends MarketParams {
  /** Risk aversion. 0 is exactly TWAP. */
  lambda: number;
}

export type StrategyId = 'TWAP' | 'VWAP' | 'POV' | 'AC';
