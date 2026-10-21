import { readFileSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import { beforeAll, describe, expect, it } from 'vitest';
import golden from './golden.json';
import {
  acWeights,
  PermanentImpact,
  llround,
  mean,
  pov,
  runBacktest,
  runWindows,
  scheduled,
  solveAc,
  stdev,
  twapWeights,
  unpack,
  vwapWeights,
  windowStarts,
  type AcParams,
  type DaySeries,
  type PackMeta,
  type Strategy,
} from './index';

const dataDir = new URL('../../public/data/', import.meta.url);
let day: DaySeries;

beforeAll(() => {
  const meta = JSON.parse(readFileSync(new URL('meta.json', dataDir), 'utf8')) as PackMeta;
  const raw = gunzipSync(readFileSync(new URL(meta.pack, dataDir)));
  day = unpack(new Uint8Array(raw), meta);
});

const G = golden as unknown as {
  params: { gamma: number; eta: number; sigma: number; epsilon: number };
  single: { config: Record<string, number>; strategies: Record<string, any> };
  windows: { config: Record<string, number>; strategies: Record<string, any[]> };
};

const acParams = (lambda: number): AcParams => ({ ...G.params, lambda });

function makeStrategies(qty: number, minutes: number, lambda: number): Record<string, Strategy> {
  return {
    TWAP: scheduled('TWAP', qty, twapWeights(minutes)),
    VWAP: scheduled('VWAP', qty, vwapWeights(minutes, 2.0)),
    'POV(5%)': pov(0.05, minutes),
    AC: scheduled('AC', qty, acWeights(minutes, minutes, acParams(lambda))),
  };
}

describe('data pack', () => {
  it('decodes a full session on a 1-second grid with ten levels', () => {
    expect(day.n).toBe(23400);
    expect(day.levels).toBe(10);
    expect(day.volPrefix[day.n]).toBeGreaterThan(1e6); // AAPL traded millions of shares
  });
});

describe('rounding', () => {
  it('rounds half away from zero like C++ llround (JS Math.round does not)', () => {
    expect(llround(2.5)).toBe(3);
    expect(llround(-2.5)).toBe(-3);
    expect(Math.round(-2.5)).toBe(-2); // the trap this helper exists to avoid
    expect(llround(-0.4)).toBe(-0);
  });
});

describe('permanent impact', () => {
  it('shifts the ladder by gamma x net shares, rounded half away from zero at query time', () => {
    // 5 shares x 0.5 ticks/share = -2.5 ticks: C++ gives -3, JS Math.round would give -2.
    const imp = new PermanentImpact(0.5);
    imp.onFill(-5);
    expect(imp.shift()).toBe(-3);
  });

  it('rounds the cumulative product, never each fill (small children must not floor to zero)', () => {
    const imp = new PermanentImpact(0.004); // each 100-share fill is 0.4 ticks
    for (let i = 0; i < 3; i++) imp.onFill(-100);
    expect(imp.shift()).toBe(-1); // llround(1.2); per-fill rounding would leave 0
  });
});

describe('Almgren-Chriss solver', () => {
  const p = acParams(1e-9);

  it('lambda = 0 is exactly TWAP', () => {
    const s = solveAc(1000, 30, 30, { ...p, lambda: 0 });
    for (let j = 0; j <= 30; j++) expect(s.holdings[j]).toBeCloseTo((1000 * (30 - j)) / 30, 9);
  });

  it('matches an independent tridiagonal (Thomas) solve of the same objective', () => {
    // Shares no code with the sinh closed form: solve the stationarity system directly.
    for (const lambda of [3e-11, 3e-10, 3e-9, 3e-8]) {
      for (const N of [10, 30, 60]) {
        const T = N;
        const q = acParams(lambda);
        const tau = T / N;
        const etaT = q.eta - 0.5 * q.gamma * tau;
        const X = 10000;
        const m = N - 1;
        const off = (-2 * etaT) / tau;
        const diag = (4 * etaT) / tau + 2 * lambda * q.sigma * q.sigma * tau;
        const c = new Array<number>(m).fill(0);
        const d = new Array<number>(m).fill(0);
        c[0] = off / diag;
        d[0] = (-off * X) / diag;
        for (let j = 1; j < m; j++) {
          const den = diag - off * c[j - 1]!;
          c[j] = off / den;
          d[j] = (0 - off * d[j - 1]!) / den;
        }
        const x = new Array<number>(N + 1).fill(0);
        x[0] = X;
        x[m] = d[m - 1]!;
        for (let j = m - 1; j >= 1; j--) x[j] = d[j - 1]! - c[j - 1]! * x[j + 1]!;
        const closed = solveAc(X, T, N, q).holdings;
        for (let j = 0; j <= N; j++) expect(Math.abs(closed[j]! - x[j]!) / X).toBeLessThan(1e-9);
      }
    }
  });

  it('is monotone, pinned at both ends, and front-loads as lambda rises', () => {
    const slow = solveAc(1, 30, 30, acParams(3e-11)).holdings;
    const fast = solveAc(1, 30, 30, acParams(3e-8)).holdings;
    expect(fast[0]).toBe(1);
    expect(fast[30]).toBe(0);
    for (let j = 1; j <= 30; j++) expect(fast[j]!).toBeLessThanOrEqual(fast[j - 1]!);
    expect(fast[10]!).toBeLessThan(slow[10]!);
  });

  it('rejects ill-posed parameters (eta~ <= 0), the INTC finding', () => {
    expect(() => solveAc(1000, 30, 30, { ...p, gamma: 100 })).toThrow(/ill-posed/);
  });
});

describe('single window reproduces the C++ backtester', () => {
  const cfg = G.single.config as {
    qty: number;
    startMin: number;
    durationMin: number;
    slices: number;
    lambda: number;
  };
  const parent = {
    qty: cfg.qty,
    startSec: cfg.startMin * 60,
    durationSec: cfg.durationMin * 60,
    slices: cfg.slices,
  };
  const names: Array<[string, string]> = [
    ['TWAP', 'TWAP'],
    ['VWAP', 'VWAP'],
    ['POV(5%)', 'POV(5%)'],
    ['AC', 'AC'],
  ];

  for (const [key, goldenKey] of names) {
    it(`${key}: per-slice integers match exactly; cost components match to print precision`, () => {
      const strat = makeStrategies(cfg.qty, cfg.durationMin, cfg.lambda)[key]!;
      const r = runBacktest(day, parent, strat, G.params.gamma);
      const g = G.single.strategies[goldenKey];

      expect(r.slices.map((s) => s.child)).toEqual(g.slices.child);
      expect(r.slices.map((s) => s.filled)).toEqual(g.slices.filled);
      expect(r.slices.map((s) => s.shift)).toEqual(g.slices.shift);
      expect(r.slices.map((s) => s.mid2xBefore)).toEqual(g.slices.mid2xBefore);

      expect(r.executed).toBe(g.executed);
      expect(r.remainder).toBe(g.remainder);
      expect(r.isBps).toBeCloseTo(g.isBps, 3);
      expect(r.driftBps).toBeCloseTo(g.driftBps, 3);
      expect(r.permanentBps).toBeCloseTo(g.permanentBps, 3);
      expect(r.executionBps).toBeCloseTo(g.executionBps, 3);
      expect(r.participation).toBeCloseTo(g.participation, 5);
    });
  }
});

describe('71 rolling windows reproduce the C++ distribution', () => {
  const cfg = G.windows.config as { qty: number; durationMin: number; slices: number };
  const base = { qty: cfg.qty, durationSec: cfg.durationMin * 60, slices: cfg.slices };

  it('uses the same 71 window starts', () => {
    expect(windowStarts(cfg.durationMin).length).toBe(71);
    expect(windowStarts(cfg.durationMin)[70]).toBe(355);
  });

  const factories: Array<[string, (p: { qty: number }) => Strategy]> = [
    ['TWAP', (p) => scheduled('TWAP', p.qty, twapWeights(cfg.slices))],
    ['VWAP', (p) => scheduled('VWAP', p.qty, vwapWeights(cfg.slices, 2.0))],
    ['POV(5%)', () => pov(0.05, cfg.slices)],
    ...[3e-11, 3e-10, 3e-9, 3e-8].map((lam): [string, (p: { qty: number }) => Strategy] => [
      `AC(${lam
        .toExponential(0)
        .replace('e-', 'e-0')
        .replace(/e-0(\d\d)/, 'e-$1')})`,
      (p) => scheduled('AC', p.qty, acWeights(cfg.slices, cfg.durationMin, acParams(lam))),
    ]),
  ];

  for (const [name, make] of factories) {
    it(`${name}: every window's IS and drift match; mean/std agree`, () => {
      const { series, results } = runWindows(day, base, make, G.params.gamma);
      const g = G.windows.strategies[name]!;
      expect(g, `golden series ${name}`).toBeDefined();
      expect(series.isBps.length).toBe(g.length);

      for (let i = 0; i < g.length; i++) {
        expect(series.startMin[i]).toBe(g[i]!.startMin);
        expect(Math.abs(series.isBps[i]! - g[i]!.isBps)).toBeLessThan(2e-3);
        expect(Math.abs(series.driftBps[i]! - g[i]!.driftBps)).toBeLessThan(2e-3);
      }
      const gIs = g.map((w: any) => w.isBps as number);
      expect(Math.abs(mean(series.isBps) - mean(gIs))).toBeLessThan(1e-3);
      expect(Math.abs(stdev(series.isBps) - stdev(gIs))).toBeLessThan(1e-3);

      // The four cost terms must sum to the total, by construction, on every window.
      for (const r of results) {
        const sum = r.driftBps + r.permanentBps + r.executionBps + r.opportunityBps;
        expect(Math.abs(sum - r.isBps)).toBeLessThan(1e-9);
      }
    });
  }
});
