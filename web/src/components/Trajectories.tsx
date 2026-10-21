import { memo } from 'react';
import { useCanvas, type CanvasSurface } from '../hooks/useCanvas';
import { META, STRATEGIES } from '../lib/strategies';
import type { StrategyId } from '../engine';
import type { WindowRun } from '../model/simulate';
import { usePlayback } from '../model/playback';

const M = { l: 40, r: 14, t: 12, b: 24 };

interface Props {
  run: WindowRun;
  focus: StrategyId;
  minutes: number;
}

function TrajectoriesImpl({ run, focus, minutes }: Props) {
  const pb = usePlayback();

  const draw = ({ ctx, w, h }: CanvasSurface) => {
    const pw = w - M.l - M.r;
    const ph = h - M.t - M.b;
    const dur = minutes * 60;
    const xOf = (s: number): number => M.l + (s / dur) * pw;
    const yOf = (pctRemaining: number): number => M.t + ph - (pctRemaining / 100) * ph;

    ctx.clearRect(0, 0, w, h);
    ctx.font = '11px "IBM Plex Mono", ui-monospace, monospace';
    ctx.textBaseline = 'middle';

    for (const v of [0, 25, 50, 75, 100]) {
      const y = Math.round(yOf(v)) + 0.5;
      ctx.strokeStyle = 'rgba(160,180,200,0.10)';
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(M.l, y);
      ctx.lineTo(M.l + pw, y);
      ctx.stroke();
      ctx.fillStyle = '#66778a';
      ctx.textAlign = 'right';
      ctx.fillText(`${v}%`, M.l - 8, y);
    }
    ctx.textAlign = 'center';
    const stepMin = minutes <= 15 ? 5 : minutes <= 30 ? 10 : 15;
    for (let m = 0; m <= minutes; m += stepMin) {
      ctx.fillStyle = '#66778a';
      ctx.fillText(`${m}m`, xOf(m * 60), h - 8);
    }

    const t = Math.min(dur, pb.t);
    // draw the focused strategy last so it sits on top
    const order = [...STRATEGIES.filter((s) => s.id !== focus), META[focus]];
    for (const s of order) {
      const r = run[s.id];
      const qty = r.parent.qty;
      const pts: Array<[number, number]> = [[0, 100]];
      for (const sl of r.slices) pts.push([sl.tSec, (sl.remainingAfter / qty) * 100]);
      const last = pts[pts.length - 1]!;
      if (last[0] < dur) pts.push([dur, last[1]]);

      const isFocus = s.id === focus;
      // ghost: where this strategy ends up
      ctx.strokeStyle = `${s.color}${isFocus ? '40' : '30'}`;
      ctx.lineWidth = 1.25;
      ctx.beginPath();
      pts.forEach(([x, y], i) => (i === 0 ? ctx.moveTo(xOf(x), yOf(y)) : ctx.lineTo(xOf(x), yOf(y))));
      ctx.stroke();

      // progress: what it has actually done so far, held flat to the playhead
      ctx.strokeStyle = s.color;
      ctx.lineWidth = isFocus ? 2.75 : 1.75;
      ctx.shadowColor = isFocus ? s.color : 'transparent';
      ctx.shadowBlur = isFocus ? 10 : 0;
      ctx.beginPath();
      let lastY = 100;
      ctx.moveTo(xOf(0), yOf(100));
      for (const [x, y] of pts) {
        if (x > t) break;
        ctx.lineTo(xOf(x), yOf(y));
        lastY = y;
      }
      ctx.lineTo(xOf(t), yOf(lastY));
      ctx.stroke();
      ctx.shadowBlur = 0;

      ctx.fillStyle = s.color;
      ctx.beginPath();
      ctx.arc(xOf(t), yOf(lastY), isFocus ? 4 : 3, 0, Math.PI * 2);
      ctx.fill();
    }

    ctx.strokeStyle = 'rgba(230,235,241,0.55)';
    ctx.lineWidth = 1;
    const px = Math.round(xOf(t)) + 0.5;
    ctx.beginPath();
    ctx.moveTo(px, M.t);
    ctx.lineTo(px, M.t + ph);
    ctx.stroke();
  };

  const { ref } = useCanvas(draw);
  return (
    <div className="stage stage--traj">
      <canvas
        ref={ref}
        className="traj"
        role="img"
        aria-label="Shares remaining over time for each strategy. The optimal schedule sells earlier as the dial moves toward urgent."
      />
    </div>
  );
}

export const Trajectories = memo(TrajectoriesImpl);
