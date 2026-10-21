import { runBacktest, type BacktestResult } from './backtest';
import type { Strategy } from './strategies';
import type { DaySeries, ParentOrder } from './types';

export const SESSION_MIN = 390;

/**
 * Start minutes of the rolling windows: every 5 minutes from minute 5, while the
 * window still ends inside the session. For a 30-minute order this is the 71
 * windows used for the README results.
 */
export function windowStarts(durationMin: number): number[] {
  const last = Math.min(355, SESSION_MIN - 5 - durationMin);
  const out: number[] = [];
  for (let m = 5; m <= last; m += 5) out.push(m);
  return out;
}

export interface WindowSeries {
  startMin: number[];
  isBps: number[];
  driftBps: number[];
  /** Impact cost = IS - drift: what the strategy controls, with the market's luck removed. */
  impactBps: number[];
}

export const mean = (xs: number[]): number => xs.reduce((a, b) => a + b, 0) / xs.length;

/** Sample standard deviation (n - 1), matching Python's statistics.stdev. */
export function stdev(xs: number[]): number {
  const m = mean(xs);
  return Math.sqrt(xs.reduce((a, b) => a + (b - m) ** 2, 0) / (xs.length - 1));
}

/** Run one strategy over every rolling window. A fresh strategy per window (they are stateful). */
export function runWindows(
  day: DaySeries,
  base: Omit<ParentOrder, 'startSec'>,
  make: (parent: ParentOrder) => Strategy,
  gamma: number,
): { series: WindowSeries; results: BacktestResult[] } {
  const durationMin = base.durationSec / 60;
  const series: WindowSeries = { startMin: [], isBps: [], driftBps: [], impactBps: [] };
  const results: BacktestResult[] = [];
  for (const m of windowStarts(durationMin)) {
    const parent: ParentOrder = { ...base, startSec: m * 60 };
    const r = runBacktest(day, parent, make(parent), gamma);
    results.push(r);
    series.startMin.push(m);
    series.isBps.push(r.isBps);
    series.driftBps.push(r.driftBps);
    series.impactBps.push(r.isBps - r.driftBps);
  }
  return { series, results };
}
