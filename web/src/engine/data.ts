import type { DaySeries } from './types';

export interface PackMeta {
  ticker: string;
  date: string;
  pack: string;
  n: number;
  levels: number;
  columns: number;
  openNs: string;
  openSecAfterMidnight: number;
}

// ---- accessors (all take a grid second `i`, level `l`) ----------------------

export const askPx = (d: DaySeries, i: number, l: number): number => d.cols[l * d.n + i]!;
export const askSz = (d: DaySeries, i: number, l: number): number => d.cols[(d.levels + l) * d.n + i]!;
export const bidPx = (d: DaySeries, i: number, l: number): number => d.cols[(2 * d.levels + l) * d.n + i]!;
export const bidSz = (d: DaySeries, i: number, l: number): number => d.cols[(3 * d.levels + l) * d.n + i]!;

/** 2 x mid in ticks (integer-exact), or 0 when either side of the book is empty. */
export const rawMid2x = (d: DaySeries, i: number): number => {
  const a = askPx(d, i, 0);
  const b = bidPx(d, i, 0);
  return a !== 0 && b !== 0 ? a + b : 0;
};

/** Market volume in grid bins [from, to), i.e. the C++ market_volume_between. */
export const marketVolume = (d: DaySeries, from: number, to: number): number =>
  d.volPrefix[Math.min(to, d.n)]! - d.volPrefix[Math.min(from, d.n)]!;

// ---- decoding ----------------------------------------------------------------

/**
 * Turn the raw (already gunzipped) pack bytes into a DaySeries. Each column was
 * stored delta-coded in time, so a running sum restores absolute values.
 */
export function unpack(raw: Uint8Array, meta: PackMeta): DaySeries {
  const { n, levels, columns } = meta;
  const expected = n * columns * 4;
  if (raw.byteLength !== expected) {
    throw new Error(`pack is ${raw.byteLength} bytes, expected ${expected}`);
  }
  // Copy to a fresh, aligned buffer: Int32Array needs 4-byte alignment.
  const cols = new Int32Array(raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.byteLength));
  for (let c = 0; c < columns; c++) {
    const base = c * n;
    let acc = 0;
    for (let i = 0; i < n; i++) {
      acc += cols[base + i]!;
      cols[base + i] = acc;
    }
  }
  const volBase = 4 * levels * n;
  const volPrefix = new Float64Array(n + 1);
  for (let i = 0; i < n; i++) volPrefix[i + 1] = volPrefix[i]! + cols[volBase + i]!;
  return { n, levels, openSec: meta.openSecAfterMidnight, cols, volPrefix };
}

const isGzip = (b: Uint8Array): boolean => b.length > 2 && b[0] === 0x1f && b[1] === 0x8b;

/** Browser loader: fetch the pack, gunzip it with DecompressionStream, decode. */
export async function loadDay(baseUrl = '/data/'): Promise<DaySeries> {
  const metaRes = await fetch(`${baseUrl}meta.json`);
  if (!metaRes.ok) throw new Error(`meta.json: HTTP ${metaRes.status}`);
  const meta = (await metaRes.json()) as PackMeta;

  const packRes = await fetch(`${baseUrl}${meta.pack}`);
  if (!packRes.ok) throw new Error(`${meta.pack}: HTTP ${packRes.status}`);
  let bytes = new Uint8Array(await packRes.arrayBuffer());

  // Some hosts transparently decode gzip; only decompress when the magic bytes say so.
  if (isGzip(bytes)) {
    const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('gzip'));
    bytes = new Uint8Array(await new Response(stream).arrayBuffer());
  }
  return unpack(bytes, meta);
}
