import {
  acWeights,
  exposure,
  mean,
  pov,
  rawMid2x,
  runBacktest,
  runWindows,
  scheduled,
  solveAc,
  stdev,
  twapWeights,
  vwapWeights,
  windowStarts,
  type AcParams,
  type BacktestResult,
  type DaySeries,
  type MarketParams,
  type ParentOrder,
  type Strategy,
  type StrategyId,
} from '../engine';

/**
 * AAPL parameters fitted from the 2012-06-21 LOBSTER sample by the C++/Python
 * calibration pipeline (results/dump/AAPL/calib.json): the exact values behind
 * the README tables.
 */
export const PARAMS: MarketParams = { gamma: 0.421774, eta: 1.13567, sigma: 2694.48, epsilon: 750 };

export const POV_PARTICIPATION = 0.05;
export const VWAP_SMILE = 2.0;

/** Log10 of the risk-aversion lambda. The dial spans four decades. */
export const LOG_LAMBDA_MIN = -11;
export const LOG_LAMBDA_MAX = -7;

export interface Config {
  qty: number;
  /** Order duration. Slices = minutes, so every slice is exactly one minute (tau = 1). */
  minutes: number;
  /** Replay window start, minutes after the open. */
  startMin: number;
  logLambda: number;
}

export const lambdaOf = (logLambda: number): number => 10 ** logLambda;

const acParams = (lambda: number): AcParams => ({ ...PARAMS, lambda });

/** Fresh strategy instance (they are stateful across the slices of one run). */
export function makeStrategy(id: StrategyId, qty: number, minutes: number, lambda: number): Strategy {
  switch (id) {
    case 'TWAP':
      return scheduled('TWAP', qty, twapWeights(minutes));
    case 'VWAP':
      return scheduled('VWAP', qty, vwapWeights(minutes, VWAP_SMILE));
    case 'POV':
      return pov(POV_PARTICIPATION, minutes);
    case 'AC':
      return scheduled('AC', qty, acWeights(minutes, minutes, acParams(lambda)));
  }
}

export const parentOf = (cfg: Pick<Config, 'qty' | 'minutes' | 'startMin'>): ParentOrder => ({
  qty: cfg.qty,
  startSec: cfg.startMin * 60,
  durationSec: cfg.minutes * 60,
  slices: cfg.minutes,
});

// ---- the replayed window ------------------------------------------------------

export type WindowRun = Record<StrategyId, BacktestResult>;

export function simulateWindow(day: DaySeries, cfg: Config): WindowRun {
  const parent = parentOf(cfg);
  const lambda = lambdaOf(cfg.logLambda);
  const run = (id: StrategyId): BacktestResult =>
    runBacktest(day, parent, makeStrategy(id, cfg.qty, cfg.minutes, lambda), PARAMS.gamma);
  return { AC: run('AC'), TWAP: run('TWAP'), VWAP: run('VWAP'), POV: run('POV') };
}

// ---- the distribution over every rolling window -------------------------------

export interface Dist {
  startMin: number[];
  isBps: number[];
  impactBps: number[];
  meanIs: number;
  stdIs: number;
  meanImpact: number;
}

function summarize(series: { startMin: number[]; isBps: number[]; impactBps: number[] }): Dist {
  return {
    startMin: series.startMin,
    isBps: series.isBps,
    impactBps: series.impactBps,
    meanIs: mean(series.isBps),
    stdIs: stdev(series.isBps),
    meanImpact: mean(series.impactBps),
  };
}

const benchCache = new Map<string, Record<'TWAP' | 'VWAP' | 'POV', Dist>>();

/** The three lambda-independent strategies over all windows. Cached per order shape. */
export function benchmarkDists(
  day: DaySeries,
  qty: number,
  minutes: number,
): Record<'TWAP' | 'VWAP' | 'POV', Dist> {
  const key = `${qty}|${minutes}`;
  const hit = benchCache.get(key);
  if (hit) return hit;
  const base = { qty, durationSec: minutes * 60, slices: minutes };
  const dist = (id: 'TWAP' | 'VWAP' | 'POV'): Dist =>
    summarize(runWindows(day, base, () => makeStrategy(id, qty, minutes, 0), PARAMS.gamma).series);
  const out = { TWAP: dist('TWAP'), VWAP: dist('VWAP'), POV: dist('POV') };
  benchCache.set(key, out);
  return out;
}

/** AC over all windows at one lambda: the part that re-runs as the dial turns (~3 ms). */
export function acDist(day: DaySeries, qty: number, minutes: number, lambda: number): Dist {
  const base = { qty, durationSec: minutes * 60, slices: minutes };
  return summarize(
    runWindows(day, base, () => makeStrategy('AC', qty, minutes, lambda), PARAMS.gamma).series,
  );
}

// ---- the frontier --------------------------------------------------------------

export interface FrontierPoint {
  logLambda: number;
  /** Timing risk, bps of arrival notional. */
  risk: number;
  /** Expected impact cost, bps of arrival notional. */
  cost: number;
}

export interface Frontiers {
  analytic: FrontierPoint[];
  realized: FrontierPoint[];
  /** Min and max per-window IS (bps) over every lambda in the sweep, so strip plots keep a fixed axis. */
  isRange: [number, number];
}

const frontierCache = new Map<string, Frontiers>();

/**
 * The model's analytic efficient frontier (closed-form E and Std of shortfall)
 * and, beside it, what the real windows actually delivered at the same lambdas:
 * realized risk = std(IS), realized cost = mean(IS - drift).
 */
export function frontiers(day: DaySeries, qty: number, minutes: number): Frontiers {
  const key = `${qty}|${minutes}`;
  const hit = frontierCache.get(key);
  if (hit) return hit;

  const starts = windowStarts(minutes);
  const m0 = mean(starts.map((m) => rawMid2x(day, m * 60) / 2));
  const notional = qty * m0;

  const analytic: FrontierPoint[] = [];
  for (let i = 0; i <= 60; i++) {
    const logLambda = LOG_LAMBDA_MIN + ((LOG_LAMBDA_MAX - LOG_LAMBDA_MIN) * i) / 60;
    const s = solveAc(qty, minutes, minutes, acParams(lambdaOf(logLambda)));
    analytic.push({
      logLambda,
      risk: (Math.sqrt(s.variance) / notional) * 1e4,
      cost: (s.expectedCost / notional) * 1e4,
    });
  }

  const realized: FrontierPoint[] = [];
  let lo = Infinity;
  let hi = -Infinity;
  for (let i = 0; i <= 12; i++) {
    const logLambda = LOG_LAMBDA_MIN + ((LOG_LAMBDA_MAX - LOG_LAMBDA_MIN) * i) / 12;
    const d = acDist(day, qty, minutes, lambdaOf(logLambda));
    realized.push({ logLambda, risk: d.stdIs, cost: d.meanImpact });
    lo = Math.min(lo, ...d.isBps);
    hi = Math.max(hi, ...d.isBps);
  }

  const out: Frontiers = { analytic, realized, isRange: [lo, hi] };
  frontierCache.set(key, out);
  return out;
}

/** Analytic risk and cost at one lambda, in the same units as the frontier. */
export function analyticAt(
  day: DaySeries,
  cfg: Pick<Config, 'qty' | 'minutes'>,
  logLambda: number,
): FrontierPoint {
  const starts = windowStarts(cfg.minutes);
  const m0 = mean(starts.map((m) => rawMid2x(day, m * 60) / 2));
  const notional = cfg.qty * m0;
  const s = solveAc(cfg.qty, cfg.minutes, cfg.minutes, acParams(lambdaOf(logLambda)));
  return {
    logLambda,
    risk: (Math.sqrt(s.variance) / notional) * 1e4,
    cost: (s.expectedCost / notional) * 1e4,
  };
}

/** How much inventory-risk AC carries relative to TWAP in the replayed window (1 = same). */
export const exposureRatio = (ac: BacktestResult, twap: BacktestResult): number =>
  exposure(ac) / exposure(twap);

/** Share of the order AC has sold by the end of the first third of the window. */
export function soldInFirstThird(ac: BacktestResult): number {
  const third = ac.parent.durationSec / 3;
  let sold = 0;
  for (const s of ac.slices) if (s.tSec < third) sold += s.filled;
  return sold / ac.parent.qty;
}
