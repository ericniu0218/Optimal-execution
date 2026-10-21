const MINUS = '−';

/** Signed fixed-point with a true minus sign: +6.62, −2.78. */
export function signed(x: number, digits = 2): string {
  if (!Number.isFinite(x)) return '—';
  const r = Number(x.toFixed(digits));
  if (r === 0) return (0).toFixed(digits);
  return `${r > 0 ? '+' : MINUS}${Math.abs(r).toFixed(digits)}`;
}

/** Unsigned fixed-point, true minus for negatives. */
export function fixed(x: number, digits = 2): string {
  if (!Number.isFinite(x)) return '—';
  const r = Number(x.toFixed(digits));
  return `${r < 0 ? MINUS : ''}${Math.abs(r).toFixed(digits)}`;
}

export const int = (x: number): string => Math.round(x).toLocaleString('en-US');

/** 3e-9 -> "3.0e-9", for the dial readout. */
export function sci(x: number): string {
  const exp = Math.floor(Math.log10(x));
  const mant = x / 10 ** exp;
  return `${mant.toFixed(1)}e${exp < 0 ? MINUS : ''}${Math.abs(exp)}`;
}

/** Seconds after midnight -> "10:45:12". */
export function clock(secAfterMidnight: number): string {
  const s = Math.max(0, Math.floor(secAfterMidnight));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  const p = (n: number): string => String(n).padStart(2, '0');
  return `${p(h)}:${p(m)}:${p(sec)}`;
}

export const clockHm = (secAfterMidnight: number): string => clock(secAfterMidnight).slice(0, 5);

/** Ticks (1e-4 dollars) -> "$586.40". */
export const dollars = (ticks: number): string => `$${(ticks / 1e4).toFixed(2)}`;

export const pct = (x: number, digits = 0): string => `${signedPlain(x * 100, digits)}%`;

function signedPlain(x: number, digits: number): string {
  const r = Number(x.toFixed(digits));
  return `${r < 0 ? MINUS : ''}${Math.abs(r).toFixed(digits)}`;
}
