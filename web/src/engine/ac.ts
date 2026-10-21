import type { AcParams } from './types';

/**
 * Exact discrete-time Almgren-Chriss liquidation (TypeScript port of
 * cpp/src/ac/solver.cpp; derivation in MODEL.md).
 *
 * Minimizing E[IS] + lambda * Var[IS] over holdings x_0..x_N gives a linear
 * recursion with decay rate
 *
 *     kappa = (2 / tau) * asinh( ktilde * tau / 2 ),   ktilde^2 = lambda sigma^2 / eta~
 *     eta~  = eta - gamma * tau / 2        (the discreteness correction)
 *
 * and the trajectory x_j = X sinh(kappa (T - t_j)) / sinh(kappa T). lambda = 0
 * is exactly the straight TWAP line, not a separate code path.
 */

/**
 * sinh(a)/sinh(b) for 0 <= a <= b, evaluated as e^(a-b) * expm1(-2a) / expm1(-2b).
 * Naive sinh overflows near kappa*T ~ 710 and cancels as kappa*T -> 0; this form
 * has neither problem because every exponent is <= 0 and expm1 is exact near 0.
 */
const sinhRatio = (a: number, b: number): number =>
  (Math.exp(a - b) * Math.expm1(-2 * a)) / Math.expm1(-2 * b);

export interface AcSolution {
  /** x_0 .. x_N, shares still held after k intervals. x_0 = X, x_N = 0. */
  holdings: number[];
  /** n_1 .. n_N. */
  trades: number[];
  /** Trajectory decay rate, 1/minute. */
  kappa: number;
  /** E[IS], ticks * shares. */
  expectedCost: number;
  /** Var[IS], (ticks * shares)^2. */
  variance: number;
}

export function expectedCost(holdings: number[], T: number, p: AcParams): number {
  const N = holdings.length - 1;
  const tau = T / N;
  const etaTilde = p.eta - 0.5 * p.gamma * tau;
  const X = holdings[0]! - holdings[N]!;
  let sumAbs = 0;
  let sumSq = 0;
  for (let k = 1; k <= N; k++) {
    const n = holdings[k - 1]! - holdings[k]!;
    sumAbs += Math.abs(n);
    sumSq += n * n;
  }
  return 0.5 * p.gamma * X * X + p.epsilon * sumAbs + (etaTilde / tau) * sumSq;
}

export function variance(holdings: number[], T: number, p: AcParams): number {
  const N = holdings.length - 1;
  const tau = T / N;
  let sumSq = 0;
  for (let k = 1; k < N; k++) sumSq += holdings[k]! * holdings[k]!;
  return p.sigma * p.sigma * tau * sumSq;
}

/**
 * Solve for the optimal trajectory. T is in minutes, N is the slice count.
 * Throws when eta~ <= 0: permanent impact then dominates temporary impact at
 * this slice length and the discrete problem is not convex (the INTC finding).
 */
export function solveAc(X: number, T: number, N: number, p: AcParams): AcSolution {
  const tau = T / N;
  const etaTilde = p.eta - 0.5 * p.gamma * tau;
  if (!(T > 0) || N < 1) throw new Error('solveAc: T must be > 0 and N >= 1');
  if (!(p.eta > 0) || p.lambda < 0 || p.sigma < 0) throw new Error('solveAc: invalid parameters');
  if (!(etaTilde > 0)) {
    throw new Error(`solveAc: eta~ = ${etaTilde.toFixed(4)} <= 0; ill-posed at this slice length`);
  }

  const ktilde = Math.sqrt((p.lambda * p.sigma * p.sigma) / etaTilde);
  const kappa = (2 / tau) * Math.asinh(0.5 * ktilde * tau);

  const holdings = new Array<number>(N + 1);
  if (kappa === 0) {
    for (let j = 0; j <= N; j++) holdings[j] = (X * (N - j)) / N;
  } else {
    const eNegKT = Math.exp(-kappa * T);
    for (let j = 0; j <= N; j++) {
      const t = tau * j;
      holdings[j] = X * Math.exp(-kappa * t) - X * eNegKT * sinhRatio(kappa * t, kappa * T);
    }
  }
  holdings[0] = X; // pin the boundaries exactly against rounding
  holdings[N] = 0;

  const trades = new Array<number>(N);
  for (let k = 1; k <= N; k++) trades[k - 1] = holdings[k - 1]! - holdings[k]!;

  return {
    holdings,
    trades,
    kappa,
    expectedCost: expectedCost(holdings, T, p),
    variance: variance(holdings, T, p),
  };
}

// ---- schedules: fraction of the parent still unexecuted at the start of slice j ----

/** w_j = 1 - j/N. */
export function twapWeights(N: number): number[] {
  return Array.from({ length: N + 1 }, (_, j) => (N - j) / N);
}

/**
 * Parametric U-shaped intraday volume curve v(u) = 1 + smile (2u - 1)^2,
 * followed through its cumulative. Parametric on purpose: fitting the curve to
 * the day being replayed would hand VWAP in-sample information.
 */
export function vwapWeights(N: number, smile: number): number[] {
  const cum = (u: number): number => {
    const m = 2 * u - 1;
    return (u + (smile * (m * m * m + 1)) / 6) / (1 + smile / 3);
  };
  const w = Array.from({ length: N + 1 }, (_, j) => 1 - cum(j / N));
  w[0] = 1;
  w[N] = 0;
  return w;
}

/** The optimal schedule: solve with X = 1 so the holdings are their own weights. */
export function acWeights(N: number, T: number, p: AcParams): number[] {
  return solveAc(1, T, N, p).holdings;
}
