import { memo, useEffect, useMemo, useRef, type PointerEvent } from 'react';
import {
  askPx,
  askSz,
  bidPx,
  bidSz,
  rawMid2x,
  type BacktestResult,
  type DaySeries,
  type ParentOrder,
  type StrategyId,
} from '../engine';
import { useCanvas, type CanvasSurface } from '../hooks/useCanvas';
import { clockHm } from '../lib/format';
import { META, STRATEGIES } from '../lib/strategies';
import { PARAMS, type WindowRun } from '../model/simulate';
import { playback, usePlayback } from '../model/playback';

const M = { l: 12, r: 64, t: 30, b: 26 };
const BID_RGB = '96,160,190';
const ASK_RGB = '176,132,222';
const INK = '#0d1219';
const GRID = 'rgba(160,180,200,0.07)';
const LABEL = '#66778a';

interface Props {
  day: DaySeries;
  parent: ParentOrder;
  run: WindowRun;
  focus: StrategyId;
}

interface YRange {
  min: number;
  max: number;
}

/** Fixed for a window and order size, so the picture does not breathe as the dial turns. */
function yRangeOf(day: DaySeries, parent: ParentOrder): YRange {
  let lo = Infinity;
  let hi = -Infinity;
  for (let s = 0; s <= parent.durationSec; s++) {
    const g = parent.startSec + s;
    const b = bidPx(day, g, 2) || bidPx(day, g, 0);
    const a = askPx(day, g, 2) || askPx(day, g, 0);
    if (b) lo = Math.min(lo, b);
    if (a) hi = Math.max(hi, a);
  }
  const span = Math.max(3000, hi - lo);
  // Room below for the largest permanent impact this order could ever cause.
  const maxShift = PARAMS.gamma * parent.qty;
  return { min: lo - maxShift - span * 0.12, max: hi + span * 0.05 };
}

/** The raw historical mid, and the mid after the focus strategy's own impact, per second. */
function midSeries(day: DaySeries, parent: ParentOrder, r: BacktestResult) {
  const n = parent.durationSec;
  const raw = new Float64Array(n + 1);
  const imp = new Float64Array(n + 1);
  let ki = -1;
  for (let s = 0; s <= n; s++) {
    while (ki + 1 < r.slices.length && r.slices[ki + 1]!.tSec <= s) ki++;
    const shift = ki < 0 ? 0 : ki + 1 < r.slices.length ? r.slices[ki + 1]!.shift : r.finalShift;
    raw[s] = rawMid2x(day, parent.startSec + s) / 2;
    imp[s] = raw[s]! + shift;
  }
  return { raw, imp };
}

function niceStep(rangeDollars: number, target: number): number {
  const raw = rangeDollars / target;
  for (const s of [0.05, 0.1, 0.2, 0.25, 0.5, 1, 2, 5]) if (s >= raw) return s;
  return 10;
}

/** Pre-render the real book as terrain: one pixel column per sample, ten levels a side. */
function renderHeat(day: DaySeries, parent: ParentOrder, yr: YRange, w: number, h: number, dpr: number) {
  const c = document.createElement('canvas');
  c.width = Math.round(w * dpr);
  c.height = Math.round(h * dpr);
  const g = c.getContext('2d')!;
  g.scale(dpr, dpr);
  const pw = w - M.l - M.r;
  const ph = h - M.t - M.b;
  const pxPerTick = ph / (yr.max - yr.min);
  const yOf = (p: number): number => M.t + ph - (p - yr.min) * pxPerTick;
  const th = Math.max(2, 100 * pxPerTick);
  const inten = (sz: number): number => Math.min(1, Math.log1p(sz) / Math.log1p(2200)) ** 1.15 * 0.98;

  for (let x = 0; x < pw; x++) {
    const s = Math.min(parent.durationSec - 1, Math.floor(((x + 0.5) / pw) * parent.durationSec));
    const gs = parent.startSec + s;
    for (let l = 0; l < day.levels; l++) {
      const ap = askPx(day, gs, l);
      if (ap) {
        g.fillStyle = `rgba(${ASK_RGB},${inten(askSz(day, gs, l)).toFixed(3)})`;
        g.fillRect(M.l + x, yOf(ap) - th / 2, 1, th);
      }
      const bp = bidPx(day, gs, l);
      if (bp) {
        g.fillStyle = `rgba(${BID_RGB},${inten(bidSz(day, gs, l)).toFixed(3)})`;
        g.fillRect(M.l + x, yOf(bp) - th / 2, 1, th);
      }
    }
  }
  return c;
}

function HeatmapImpl({ day, parent, run, focus }: Props) {
  const pb = usePlayback();
  const heatRef = useRef<{ key: string; canvas: HTMLCanvasElement } | null>(null);
  const yr = useMemo(() => yRangeOf(day, parent), [day, parent]);
  const mids = useMemo(() => midSeries(day, parent, run[focus]), [day, parent, run, focus]);

  const draw = ({ ctx, w, h, dpr }: CanvasSurface) => {
    const pw = w - M.l - M.r;
    const ph = h - M.t - M.b;
    const dur = parent.durationSec;
    const xOf = (s: number): number => M.l + (s / dur) * pw;
    const yOf = (p: number): number => M.t + ph - ((p - yr.min) / (yr.max - yr.min)) * ph;

    ctx.fillStyle = INK;
    ctx.fillRect(0, 0, w, h);

    // terrain
    const key = `${parent.startSec}|${dur}|${parent.qty}|${w}|${h}|${dpr}`;
    if (!heatRef.current || heatRef.current.key !== key) {
      heatRef.current = { key, canvas: renderHeat(day, parent, yr, w, h, dpr) };
    }
    ctx.drawImage(heatRef.current.canvas, 0, 0, w, h);

    ctx.font = '11px "IBM Plex Mono", ui-monospace, monospace';
    ctx.textBaseline = 'middle';

    // price grid and labels (right)
    const dMin = yr.min / 1e4;
    const dMax = yr.max / 1e4;
    const step = niceStep(dMax - dMin, 6);
    ctx.lineWidth = 1;
    for (let p = Math.ceil(dMin / step) * step; p <= dMax; p += step) {
      const y = Math.round(yOf(p * 1e4)) + 0.5;
      ctx.strokeStyle = GRID;
      ctx.beginPath();
      ctx.moveTo(M.l, y);
      ctx.lineTo(M.l + pw, y);
      ctx.stroke();
      ctx.fillStyle = LABEL;
      ctx.textAlign = 'left';
      ctx.fillText(`$${p.toFixed(2)}`, M.l + pw + 8, y);
    }
    // time grid and labels (bottom); the end labels hug the plot edge instead of clipping
    const tStep = dur <= 1200 ? 120 : dur <= 2100 ? 300 : 600;
    for (let s = 0; s <= dur; s += tStep) {
      const x = Math.round(xOf(s)) + 0.5;
      ctx.strokeStyle = GRID;
      ctx.beginPath();
      ctx.moveTo(x, M.t);
      ctx.lineTo(x, M.t + ph);
      ctx.stroke();
      ctx.fillStyle = LABEL;
      ctx.textAlign = s === 0 ? 'left' : s + tStep > dur ? 'right' : 'center';
      ctx.fillText(clockHm(day.openSec + parent.startSec + s), s === 0 ? M.l : x, h - 11);
    }

    const t = pb.t;
    const tSec = Math.min(dur, Math.max(0, t));
    ctx.save();
    ctx.beginPath();
    ctx.rect(M.l, M.t, pw, ph);
    ctx.clip();

    // arrival price: the benchmark every shortfall number is measured against
    const arrival = run[focus].arrivalMid;
    ctx.setLineDash([3, 4]);
    ctx.strokeStyle = 'rgba(230,235,241,0.35)';
    ctx.beginPath();
    ctx.moveTo(M.l, yOf(arrival));
    ctx.lineTo(M.l + pw, yOf(arrival));
    ctx.stroke();
    ctx.setLineDash([]);

    // raw historical mid: faint for the whole window, bright up to the playhead
    const path = (arr: Float64Array, to: number): void => {
      ctx.beginPath();
      for (let s = 0; s <= to; s++) {
        const x = xOf(s);
        const y = yOf(arr[s]!);
        if (s === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
    };
    ctx.lineWidth = 1;
    ctx.strokeStyle = 'rgba(230,235,241,0.20)';
    path(mids.raw, dur);
    ctx.stroke();
    const upTo = Math.floor(tSec);
    ctx.strokeStyle = 'rgba(230,235,241,0.92)';
    ctx.lineWidth = 1.25;
    path(mids.raw, upTo);
    ctx.stroke();

    // the focus strategy's own impact: a band between where the market was and where it pushed it
    const fc = META[focus].color;
    if (upTo > 0) {
      ctx.beginPath();
      for (let s = 0; s <= upTo; s++) ctx.lineTo(xOf(s), yOf(mids.raw[s]!));
      for (let s = upTo; s >= 0; s--) ctx.lineTo(xOf(s), yOf(mids.imp[s]!));
      ctx.closePath();
      ctx.fillStyle = `${fc}2e`;
      ctx.fill();
      ctx.strokeStyle = fc;
      ctx.lineWidth = 1.5;
      path(mids.imp, upTo);
      ctx.stroke();
    }

    // every strategy's fill prices; the focus one in full detail
    const maxFill = Math.max(1, ...STRATEGIES.flatMap((s) => run[s.id].slices.map((r) => r.filled)));
    for (const s of STRATEGIES) {
      if (s.id === focus) continue;
      const fills = run[s.id].slices.filter((r) => r.filled > 0 && r.tSec <= tSec);
      if (!fills.length) continue;
      ctx.strokeStyle = `${s.color}99`;
      ctx.lineWidth = 1.25;
      ctx.beginPath();
      fills.forEach((r, i) =>
        i === 0 ? ctx.moveTo(xOf(r.tSec), yOf(r.vwapTicks)) : ctx.lineTo(xOf(r.tSec), yOf(r.vwapTicks)),
      );
      ctx.stroke();
      ctx.fillStyle = s.color;
      for (const r of fills) {
        ctx.beginPath();
        ctx.arc(xOf(r.tSec), yOf(r.vwapTicks), 2.2, 0, Math.PI * 2);
        ctx.fill();
      }
    }

    const reduce = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    const focusFills = run[focus].slices.filter((r) => r.filled > 0 && r.tSec <= tSec);
    for (const r of focusFills) {
      const x = xOf(r.tSec);
      const bestBid = bidPx(day, r.gridSec, 0) + r.shift;
      // depth consumed: from the touch to the deepest level reached
      ctx.strokeStyle = fc;
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(x, yOf(bestBid));
      ctx.lineTo(x, yOf(r.worstPx));
      ctx.stroke();
      const rad = 3 + 7 * Math.sqrt(r.filled / maxFill);
      ctx.fillStyle = fc;
      ctx.strokeStyle = '#0d1219';
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      ctx.arc(x, yOf(r.vwapTicks), rad, 0, Math.PI * 2);
      ctx.fill();
      ctx.stroke();
      // a ring that expands and fades as replay time passes the fill
      const age = (tSec - r.tSec) / (dur * 0.03);
      if (!reduce && age >= 0 && age < 1) {
        ctx.strokeStyle = `${fc}${Math.round((1 - age) * 200)
          .toString(16)
          .padStart(2, '0')}`;
        ctx.lineWidth = 1.5;
        ctx.beginPath();
        ctx.arc(x, yOf(r.vwapTicks), rad + age * 22, 0, Math.PI * 2);
        ctx.stroke();
      }
    }
    ctx.restore();

    // playhead
    const px = Math.round(xOf(tSec)) + 0.5;
    ctx.strokeStyle = 'rgba(230,235,241,0.85)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(px, M.t - 6);
    ctx.lineTo(px, M.t + ph);
    ctx.stroke();
    const label =
      clockHm(day.openSec + parent.startSec + tSec) + ':' + String(Math.floor(tSec) % 60).padStart(2, '0');
    ctx.font = '600 11px "IBM Plex Mono", ui-monospace, monospace';
    const tw = ctx.measureText(label).width + 12;
    const lx = Math.min(M.l + pw - tw / 2, Math.max(M.l + tw / 2, px));
    ctx.fillStyle = '#e6ebf1';
    ctx.fillRect(lx - tw / 2, 4, tw, 17);
    ctx.fillStyle = '#0b0e13';
    ctx.textAlign = 'center';
    ctx.fillText(label, lx, 13);
  };

  const { ref, paint } = useCanvas(draw);

  // Canvas text needs the web fonts; repaint once they have loaded.
  useEffect(() => {
    void document.fonts?.ready.then(() => paint());
  }, [paint]);

  const seekFromEvent = (e: PointerEvent<HTMLCanvasElement>) => {
    const rect = e.currentTarget.getBoundingClientRect();
    const pw = rect.width - M.l - M.r;
    const frac = Math.min(1, Math.max(0, (e.clientX - rect.left - M.l) / pw));
    playback.seek(frac * parent.durationSec);
  };

  return (
    <div className="stage stage--heat">
      <canvas
        ref={ref}
        className="heat"
        role="img"
        aria-label="Liquidity heatmap of the real AAPL order book during the replayed window, with each strategy's fills drawn through it. Drag to scrub through time."
        onPointerDown={(e) => {
          e.currentTarget.setPointerCapture(e.pointerId);
          playback.pause();
          seekFromEvent(e);
        }}
        onPointerMove={(e) => {
          if (e.currentTarget.hasPointerCapture(e.pointerId)) seekFromEvent(e);
        }}
      />
    </div>
  );
}

export const Heatmap = memo(HeatmapImpl);
