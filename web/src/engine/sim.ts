import { askPx, bidPx, bidSz } from './data';
import type { DaySeries } from './types';

/**
 * C++ std::llround: round half AWAY from zero. JavaScript's Math.round rounds
 * half toward +infinity, which differs for negative halves (-2.5 -> -2, not -3).
 * The ladder shift is negative for a sell program, so this matters.
 */
export const llround = (x: number): number => (x < 0 ? -Math.round(-x) : Math.round(x));

/**
 * Permanent impact: the whole replayed ladder shifts by gamma * (net signed
 * shares), rounded ONCE at query time so small children are not floored to
 * zero one fill at a time. Integer ticks keep prices on the exchange lattice.
 */
export class PermanentImpact {
  private cumSigned = 0;
  constructor(private readonly gamma: number) {}
  shift(): number {
    return llround(this.gamma * this.cumSigned);
  }
  /** signed > 0 bought, < 0 sold. */
  onFill(signed: number): void {
    this.cumSigned += signed;
  }
}

/** One executed child order. Prices are on the SHIFTED ladder. */
export interface Fill {
  requested: number;
  filled: number;
  /** sum(price * qty), ticks * shares. */
  notional: number;
  /** Deepest level touched, shifted ticks (0 if nothing filled). */
  worstPx: number;
  levelsTouched: number;
  /** 2 x shifted mid just before the fill; 0 if the book was one-sided. */
  mid2xBefore: number;
}

/**
 * Marketable SELL against the book prevailing at grid second `sec`: walk the
 * bid ladder best-first on the shifted prices. Partially fills if visible depth
 * runs out; the remainder stays in the caller's inventory. Hidden liquidity is
 * never assumed, which biases simulated cost upward (conservative).
 */
export function executeSell(day: DaySeries, sec: number, qty: number, shift: number): Fill {
  const a0 = askPx(day, sec, 0);
  const b0 = bidPx(day, sec, 0);
  const fill: Fill = {
    requested: qty,
    filled: 0,
    notional: 0,
    worstPx: 0,
    levelsTouched: 0,
    mid2xBefore: a0 !== 0 && b0 !== 0 ? a0 + shift + (b0 + shift) : 0,
  };
  let remaining = qty;
  for (let l = 0; l < day.levels && remaining > 0; l++) {
    const px = bidPx(day, sec, l);
    const depth = bidSz(day, sec, l);
    if (px === 0 || depth <= 0) break;
    const take = Math.min(remaining, depth);
    fill.notional += (px + shift) * take;
    fill.worstPx = px + shift;
    fill.levelsTouched += 1;
    fill.filled += take;
    remaining -= take;
  }
  return fill;
}
