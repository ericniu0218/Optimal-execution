import { memo, useMemo } from 'react';
import { fixed, sci, signed } from '../lib/format';
import { META } from '../lib/strategies';
import { lambdaOf, type Dist, type FrontierPoint } from '../model/simulate';

// ---- shared helpers -----------------------------------------------------------------

const scale =
  (d0: number, d1: number, r0: number, r1: number) =>
  (v: number): number =>
    r0 + ((v - d0) / (d1 - d0)) * (r1 - r0);

function ticksFor(lo: number, hi: number, target: number): number[] {
  const raw = (hi - lo) / target;
  const mag = 10 ** Math.floor(Math.log10(raw));
  const step = [1, 2, 2.5, 5, 10].map((m) => m * mag).find((s) => s >= raw) ?? raw;
  const out: number[] = [];
  for (let v = Math.ceil(lo / step) * step; v <= hi + 1e-9; v += step) out.push(Number(v.toFixed(6)));
  return out;
}

const path = (pts: Array<[number, number]>): string =>
  pts.map(([x, y], i) => `${i ? 'L' : 'M'}${x.toFixed(1)} ${y.toFixed(1)}`).join(' ');

// ---- the efficient frontier ---------------------------------------------------------

interface FrontierProps {
  analytic: FrontierPoint[];
  realized: FrontierPoint[];
  /** TWAP / VWAP / POV: realized risk and cost across every window. */
  bench: Array<{ id: 'TWAP' | 'VWAP' | 'POV'; risk: number; cost: number }>;
  /** AC at the dial: realized (from the windows) and the model's prediction. */
  ac: { risk: number; cost: number };
  model: { risk: number; cost: number };
  logLambda: number;
}

function FrontierImpl({ analytic, realized, bench, ac, model, logLambda }: FrontierProps) {
  const W = 600;
  const H = 380;
  const m = { l: 56, r: 24, t: 20, b: 52 };

  const { xs, ys, xTicks, yTicks, xMax, yLo, yHi } = useMemo(() => {
    const all = [...analytic, ...realized];
    const rx = [...all.map((p) => p.risk), ...bench.map((b) => b.risk)];
    const ry = [...all.map((p) => p.cost), ...bench.map((b) => b.cost)];
    const xMaxV = Math.max(...rx) * 1.06;
    const lo = Math.min(...ry);
    const hi = Math.max(...ry);
    const pad = (hi - lo) * 0.1;
    const yLoV = lo - pad;
    const yHiV = hi + pad;
    return {
      xs: scale(0, xMaxV, m.l, W - m.r),
      ys: scale(yLoV, yHiV, H - m.b, m.t),
      xTicks: ticksFor(0, xMaxV, 6),
      yTicks: ticksFor(yLoV, yHiV, 6),
      xMax: xMaxV,
      yLo: yLoV,
      yHi: yHiV,
    };
  }, [analytic, realized, bench, m.l, m.r, m.t, m.b]);

  const analyticPath = path(analytic.map((p) => [xs(p.risk), ys(p.cost)]));
  const realizedPath = path(realized.map((p) => [xs(p.risk), ys(p.cost)]));

  return (
    <figure className="chart">
      <svg
        viewBox={`0 0 ${W} ${H}`}
        className="chart__svg"
        role="img"
        aria-label="Efficient frontier: expected impact cost against timing risk. The model's curve and the realized results from 71 real windows, with the dial's current setting highlighted."
      >
        {yTicks.map((v) => (
          <g key={`y${v}`}>
            <line x1={m.l} x2={W - m.r} y1={ys(v)} y2={ys(v)} className="chart__grid" />
            <text x={m.l - 10} y={ys(v)} className="chart__tick" textAnchor="end" dominantBaseline="central">
              {fixed(v, 1)}
            </text>
          </g>
        ))}
        {xTicks.map((v) => (
          <g key={`x${v}`}>
            <line x1={xs(v)} x2={xs(v)} y1={m.t} y2={H - m.b} className="chart__grid" />
            <text x={xs(v)} y={H - m.b + 18} className="chart__tick" textAnchor="middle">
              {fixed(v, 0)}
            </text>
          </g>
        ))}
        <text x={(m.l + W - m.r) / 2} y={H - 10} className="chart__axis" textAnchor="middle">
          Timing risk: std of cost across windows (bps)
        </text>
        <text
          transform={`translate(14 ${(m.t + H - m.b) / 2}) rotate(-90)`}
          className="chart__axis"
          textAnchor="middle"
        >
          Impact cost, drift removed (bps)
        </text>

        <path d={analyticPath} className="chart__model" />
        <path d={realizedPath} className="chart__realized" />
        {realized.map((p) => (
          <circle key={p.logLambda} cx={xs(p.risk)} cy={ys(p.cost)} r="3" className="chart__realized-dot" />
        ))}

        {bench.map((b) => (
          <g key={b.id}>
            <circle cx={xs(b.risk)} cy={ys(b.cost)} r="6" fill={META[b.id].color} className="chart__bench" />
            <text
              x={xs(b.risk) + (b.id === 'VWAP' ? -11 : 11)}
              y={ys(b.cost) + (b.id === 'TWAP' ? 17 : -9)}
              textAnchor={b.id === 'VWAP' ? 'end' : 'start'}
              className="chart__label"
              fill={META[b.id].color}
            >
              {META[b.id].short}
            </text>
          </g>
        ))}

        {/* the dial: the model's prediction, and what the real windows delivered */}
        <line
          x1={xs(model.risk)}
          y1={ys(model.cost)}
          x2={xs(ac.risk)}
          y2={ys(ac.cost)}
          className="chart__gap"
        />
        <circle cx={xs(model.risk)} cy={ys(model.cost)} r="7" className="chart__model-pt" />
        <circle cx={xs(ac.risk)} cy={ys(ac.cost)} r="13" fill={META.AC.color} className="chart__halo" />
        <circle cx={xs(ac.risk)} cy={ys(ac.cost)} r="7.5" fill={META.AC.color} className="chart__ac" />
        <text x={xs(ac.risk) + 14} y={ys(ac.cost) - 12} className="chart__label chart__label--ac">
          AC at λ = {sci(lambdaOf(logLambda))}
        </text>

        <rect x={m.l} y={m.t} width={W - m.l - m.r} height={H - m.t - m.b} className="chart__frame" />
        <text x={W - m.r - 6} y={m.t + 14} textAnchor="end" className="chart__hint">
          less risk ← → more risk
        </text>
        <title>{`x 0 to ${fixed(xMax, 1)} bps, y ${fixed(yLo, 1)} to ${fixed(yHi, 1)} bps`}</title>
      </svg>
      <figcaption className="chart__legend">
        <span>
          <i className="lg lg--model" /> Model frontier (closed form)
        </span>
        <span>
          <i className="lg lg--real" /> Realized on the real windows
        </span>
        <span>
          <i className="lg lg--ring" /> Model at the dial
        </span>
      </figcaption>
    </figure>
  );
}
export const Frontier = memo(FrontierImpl);

// ---- the distribution strip ---------------------------------------------------------

interface DistProps {
  rows: Array<{ id: 'AC' | 'TWAP' | 'VWAP' | 'POV'; dist: Dist }>;
  /** Stable x-domain so dots slide instead of the axis rescaling. */
  domain: [number, number];
  /** Start minute of the window currently being replayed. */
  selected: number;
  minutes: number;
}

function DistImpl({ rows, domain, selected, minutes }: DistProps) {
  const W = 600;
  const rowH = 74;
  const m = { l: 14, r: 14, t: 14, b: 40 };
  const H = m.t + rows.length * rowH + m.b;
  const xs = scale(domain[0], domain[1], m.l, W - m.r);
  const xTicks = ticksFor(domain[0], domain[1], 8);

  return (
    <figure className="chart">
      <svg
        viewBox={`0 0 ${W} ${H}`}
        className="chart__svg"
        role="img"
        aria-label="Cost of each strategy in each of the 71 real windows, as a strip of dots with mean and spread. The optimal strategy's dots tighten as the dial moves toward urgent."
      >
        {xTicks.map((v) => (
          <g key={v}>
            <line
              x1={xs(v)}
              x2={xs(v)}
              y1={m.t}
              y2={H - m.b}
              className={v === 0 ? 'chart__zero' : 'chart__grid'}
            />
            <text x={xs(v)} y={H - m.b + 18} className="chart__tick" textAnchor="middle">
              {fixed(v, 0)}
            </text>
          </g>
        ))}
        <text x={(m.l + W - m.r) / 2} y={H - 8} className="chart__axis" textAnchor="middle">
          Implementation shortfall, one dot per window (bps; lower is better)
        </text>

        {rows.map((row, ri) => {
          const cy = m.t + ri * rowH + rowH / 2;
          const color = META[row.id].color;
          const { dist } = row;
          return (
            <g key={row.id}>
              <text x={m.l} y={cy - rowH / 2 + 15} className="chart__label" fill={color}>
                {row.id === 'AC' ? 'AC (dial)' : META[row.id].short}
              </text>
              {dist.isBps.map((v, i) => {
                // deterministic jitter so the strip is stable between renders
                const j = (((i * 2654435761) >>> 0) % 1000) / 1000 - 0.5;
                const isSel = dist.startMin[i] === selected;
                return (
                  <circle
                    key={dist.startMin[i]}
                    cx={xs(v)}
                    cy={cy + 6 + j * (rowH - 36)}
                    r={isSel ? 4.2 : 2.7}
                    fill={color}
                    className={isSel ? 'dot dot--sel' : 'dot'}
                  />
                );
              })}
              <line
                x1={xs(dist.meanIs - dist.stdIs)}
                x2={xs(dist.meanIs + dist.stdIs)}
                y1={cy + 6}
                y2={cy + 6}
                stroke={color}
                className="chart__band"
              />
              <path
                d={`M${xs(dist.meanIs)} ${cy - 3} l5 9 l-5 9 l-5 -9z`}
                fill={color}
                className="chart__mean"
              />
              <text x={W - m.r} y={cy - rowH / 2 + 15} textAnchor="end" className="chart__stat">
                mean {signed(dist.meanIs, 1)} · spread ±{fixed(dist.stdIs, 1)}
              </text>
            </g>
          );
        })}
      </svg>
      <figcaption className="chart__legend">
        <span>
          <i className="lg lg--dot" /> one real {minutes}-minute window
        </span>
        <span>
          <i className="lg lg--ringdot" /> the window being replayed above
        </span>
        <span>
          <i className="lg lg--mean" /> mean, with ±1 std band
        </span>
      </figcaption>
    </figure>
  );
}
export const Distribution = memo(DistImpl);
