import { memo, useCallback, useRef, type KeyboardEvent, type PointerEvent, type ReactElement } from 'react';
import { sci } from '../lib/format';
import { LOG_LAMBDA_MAX, LOG_LAMBDA_MIN, lambdaOf } from '../model/simulate';

const SWEEP = 270; // degrees of travel
const START = -135; // degrees clockwise from straight up
const CX = 130;
const CY = 130;
const R_ARC = 98;

const toT = (logL: number): number => (logL - LOG_LAMBDA_MIN) / (LOG_LAMBDA_MAX - LOG_LAMBDA_MIN);
const toLog = (t: number): number => LOG_LAMBDA_MIN + t * (LOG_LAMBDA_MAX - LOG_LAMBDA_MIN);
const angleOf = (t: number): number => START + t * SWEEP;

function polar(r: number, deg: number): [number, number] {
  const a = (deg * Math.PI) / 180;
  return [CX + r * Math.sin(a), CY - r * Math.cos(a)];
}

function arcPath(r: number, fromDeg: number, toDeg: number): string {
  const [x0, y0] = polar(r, fromDeg);
  const [x1, y1] = polar(r, toDeg);
  const large = Math.abs(toDeg - fromDeg) > 180 ? 1 : 0;
  return `M ${x0.toFixed(2)} ${y0.toFixed(2)} A ${r} ${r} 0 ${large} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`;
}

export const PRESETS: Array<{ label: string; logLambda: number; lambda: string }> = [
  { label: 'Patient', logLambda: Math.log10(3e-11), lambda: '3e-11' },
  { label: 'Measured', logLambda: Math.log10(3e-10), lambda: '3e-10' },
  { label: 'Brisk', logLambda: Math.log10(3e-9), lambda: '3e-9' },
  { label: 'Urgent', logLambda: Math.log10(3e-8), lambda: '3e-8' },
];

interface Props {
  logLambda: number;
  /** Continuous change while dragging or using the keyboard. */
  onChange: (logLambda: number) => void;
  /** Jump to a preset; the parent animates the move. */
  onPreset: (logLambda: number) => void;
}

function DialImpl({ logLambda, onChange, onPreset }: Props) {
  const svgRef = useRef<SVGSVGElement>(null);
  const lastT = useRef(toT(logLambda));
  const t = toT(logLambda);
  const deg = angleOf(t);
  // Always step from the live value, so rapid key repeats compound correctly even
  // before React has re-rendered with the previous step.
  lastT.current = t;

  const fromPointer = useCallback(
    (e: PointerEvent<SVGSVGElement>) => {
      const svg = svgRef.current;
      if (!svg) return;
      const r = svg.getBoundingClientRect();
      const dx = e.clientX - (r.left + r.width / 2);
      const dy = e.clientY - (r.top + r.height / 2);
      const theta = (Math.atan2(dx, -dy) * 180) / Math.PI; // 0 = up, clockwise positive
      let next: number;
      if (theta >= START && theta <= START + SWEEP) {
        next = (theta - START) / SWEEP;
      } else {
        // Pointer is in the dead zone at the bottom: stick to the nearer end of travel.
        next = lastT.current > 0.5 ? 1 : 0;
      }
      // Never jump across the dead zone in a single move.
      if (Math.abs(next - lastT.current) > 0.5) next = lastT.current > 0.5 ? 1 : 0;
      lastT.current = next;
      onChange(toLog(next));
    },
    [onChange],
  );

  const onKeyDown = (e: KeyboardEvent<SVGSVGElement>) => {
    const step = 0.05;
    let next = toLog(lastT.current);
    switch (e.key) {
      case 'ArrowRight':
      case 'ArrowUp':
        next += step;
        break;
      case 'ArrowLeft':
      case 'ArrowDown':
        next -= step;
        break;
      case 'PageUp':
        next += 0.5;
        break;
      case 'PageDown':
        next -= 0.5;
        break;
      case 'Home':
        next = LOG_LAMBDA_MIN;
        break;
      case 'End':
        next = LOG_LAMBDA_MAX;
        break;
      default:
        return;
    }
    e.preventDefault();
    const clamped = Math.min(LOG_LAMBDA_MAX, Math.max(LOG_LAMBDA_MIN, next));
    lastT.current = toT(clamped);
    onChange(clamped);
  };

  // Tick marks: every tenth of a decade, longer at each whole decade.
  const ticks: ReactElement[] = [];
  const decades = LOG_LAMBDA_MAX - LOG_LAMBDA_MIN;
  for (let i = 0; i <= decades * 10; i++) {
    const major = i % 10 === 0;
    const a = angleOf(i / (decades * 10));
    const [x0, y0] = polar(R_ARC + 9, a);
    const [x1, y1] = polar(R_ARC + (major ? 20 : 14), a);
    const lit = i / (decades * 10) <= t + 1e-9;
    ticks.push(
      <line
        key={i}
        x1={x0}
        y1={y0}
        x2={x1}
        y2={y1}
        className={`dial__tick${major ? ' dial__tick--major' : ''}${lit ? ' dial__tick--lit' : ''}`}
      />,
    );
  }
  const labels = Array.from({ length: decades + 1 }, (_, i) => {
    const [x, y] = polar(R_ARC + 30, angleOf(i / decades));
    return (
      <text key={i} x={x} y={y} className="dial__label" textAnchor="middle" dominantBaseline="central">
        1e{'−'}
        {Math.abs(LOG_LAMBDA_MIN + i)}
      </text>
    );
  });

  const [ix0, iy0] = polar(26, deg);
  const [ix1, iy1] = polar(54, deg);
  const [hx, hy] = polar(R_ARC, deg);

  return (
    <div className="dial">
      <svg
        ref={svgRef}
        className="dial__svg"
        viewBox="-16 -16 292 274"
        role="slider"
        tabIndex={0}
        aria-label="Risk aversion, lambda"
        aria-valuemin={LOG_LAMBDA_MIN}
        aria-valuemax={LOG_LAMBDA_MAX}
        aria-valuenow={Number(logLambda.toFixed(2))}
        aria-valuetext={`lambda ${sci(lambdaOf(logLambda))}. Patient to the left, urgent to the right.`}
        onPointerDown={(e) => {
          e.currentTarget.setPointerCapture(e.pointerId);
          fromPointer(e);
        }}
        onPointerMove={(e) => {
          if (e.currentTarget.hasPointerCapture(e.pointerId)) fromPointer(e);
        }}
        onKeyDown={onKeyDown}
      >
        <defs>
          <radialGradient id="dial-cap" cx="38%" cy="30%" r="85%">
            <stop offset="0%" stopColor="var(--cap-hi)" />
            <stop offset="100%" stopColor="var(--cap-lo)" />
          </radialGradient>
          <filter id="dial-glow" x="-30%" y="-30%" width="160%" height="160%">
            <feGaussianBlur stdDeviation="3.2" result="b" />
            <feMerge>
              <feMergeNode in="b" />
              <feMergeNode in="SourceGraphic" />
            </feMerge>
          </filter>
        </defs>

        {ticks}
        {labels}

        <path d={arcPath(R_ARC, START, START + SWEEP)} className="dial__track" />
        <path
          d={arcPath(R_ARC, START, Math.max(START + 0.01, deg))}
          className="dial__arc"
          filter="url(#dial-glow)"
        />
        <circle cx={hx} cy={hy} r="6.5" className="dial__handle" filter="url(#dial-glow)" />

        <circle cx={CX} cy={CY} r="66" className="dial__cap-ring" />
        <circle cx={CX} cy={CY} r="60" fill="url(#dial-cap)" className="dial__cap" />
        <line x1={ix0} y1={iy0} x2={ix1} y2={iy1} className="dial__pointer" />

        <text x={CX} y={CY - 8} className="dial__sym" textAnchor="middle">
          λ
        </text>
        <text x={CX} y={CY + 18} className="dial__value" textAnchor="middle">
          {sci(lambdaOf(logLambda))}
        </text>
        <text x={40} y={248} className="dial__end" textAnchor="middle">
          PATIENT
        </text>
        <text x={220} y={248} className="dial__end" textAnchor="middle">
          URGENT
        </text>
      </svg>

      <div className="presets" role="group" aria-label="Dial presets">
        {PRESETS.map((p) => {
          const active = Math.abs(p.logLambda - logLambda) < 0.04;
          return (
            <button
              key={p.label}
              type="button"
              className={`preset${active ? ' preset--on' : ''}`}
              aria-pressed={active}
              onClick={() => onPreset(p.logLambda)}
            >
              <span className="preset__name">{p.label}</span>
              <span className="preset__val">{p.lambda}</span>
            </button>
          );
        })}
      </div>
    </div>
  );
}

export const Dial = memo(DialImpl);
