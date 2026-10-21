import { memo } from 'react';
import { fixed, pct, signed } from '../lib/format';
import { STRATEGIES, META } from '../lib/strategies';
import type { StrategyId } from '../engine';
import type { WindowRun } from '../model/simulate';
import { exposureRatio } from '../model/simulate';
import { usePlayback } from '../model/playback';
import { impactOf, runningAt } from '../model/running';

interface Props {
  run: WindowRun;
  focus: StrategyId;
  /** How many real windows the distribution below averages over. */
  windows: number;
  onFocus: (id: StrategyId) => void;
}

function ScoreboardImpl({ run, focus, windows: n, onFocus }: Props) {
  const pb = usePlayback();
  const t = pb.t;
  const done = t >= pb.duration;

  // A stable frame for the bars: the largest stack any strategy ends up with.
  let domain = 5;
  for (const s of STRATEGIES) {
    const r = run[s.id];
    const pos = Math.max(0, r.driftBps) + Math.max(0, r.permanentBps) + Math.max(0, r.executionBps);
    const neg = Math.min(0, r.driftBps);
    domain = Math.max(domain, pos, -neg);
  }
  domain = Math.ceil(domain / 5) * 5;
  const pctOf = (bps: number): number => (Math.abs(bps) / domain) * 50;

  const ac = run.AC;
  const tw = run.TWAP;
  const dImpact = impactOf(ac) - impactOf(tw);
  const dDrift = ac.driftBps - tw.driftBps;
  const risk = 1 - exposureRatio(ac, tw);

  return (
    <div className="score">
      <ul className="score__rows">
        {STRATEGIES.map((s) => {
          const r = run[s.id];
          const k = runningAt(r, t);
          // positive components stack right of centre in a fixed order; negative drift goes left
          let right = 50;
          const seg = (bps: number, cls: string) => {
            if (bps <= 0) return null;
            const left = right;
            const width = pctOf(bps);
            right += width;
            return (
              <span
                key={cls}
                className={`bar__seg bar__seg--${cls}`}
                style={{ left: `${left}%`, width: `${width}%` }}
              />
            );
          };
          const driftNeg = k.drift < 0 ? pctOf(k.drift) : 0;
          const on = s.id === focus;
          return (
            <li key={s.id}>
              <button
                type="button"
                className={`row${on ? ' row--on' : ''}`}
                aria-pressed={on}
                onClick={() => onFocus(s.id)}
                style={{ ['--strat' as string]: s.color }}
              >
                <span className="row__head">
                  <span className="row__dot" />
                  <span className="row__name">{s.label}</span>
                  <span className="row__sold">sold {fixed(k.soldFrac * 100, 0)}%</span>
                  <span className="row__num">{signed(k.total, 2)}</span>
                </span>
                <span className="bar" aria-hidden="true">
                  <span className="bar__zero" />
                  {driftNeg > 0 && (
                    <span
                      className="bar__seg bar__seg--drift"
                      style={{ left: `${50 - driftNeg}%`, width: `${driftNeg}%` }}
                    />
                  )}
                  {seg(k.perm, 'perm')}
                  {seg(k.exec + k.opp, 'exec')}
                  {seg(k.drift, 'drift')}
                </span>
              </button>
            </li>
          );
        })}
      </ul>

      <dl className="key">
        <div>
          <dt className="key__sw key__sw--drift" />
          <dd>
            <b>Market drift</b> <span>price moving on its own: luck</span>
          </dd>
        </div>
        <div>
          <dt className="key__sw key__sw--perm" />
          <dd>
            <b>Permanent impact</b> <span>our selling pushed the price</span>
          </dd>
        </div>
        <div>
          <dt className="key__sw key__sw--exec" />
          <dd>
            <b>Execution</b> <span>spread and depth we walked</span>
          </dd>
        </div>
      </dl>

      <p className="score__read" aria-live="polite">
        <span className="score__tag">{done ? 'Final read' : 'Full window'}</span> Against TWAP, the optimal
        schedule paid{' '}
        <b>
          {fixed(Math.abs(dImpact), 1)} bps {dImpact >= 0 ? 'more' : 'less'}
        </b>{' '}
        in market impact and carried{' '}
        <b>
          {pct(Math.abs(risk))} {risk >= 0 ? 'less' : 'more'}
        </b>{' '}
        timing risk. Then the market&apos;s own drift in this window moved the result{' '}
        <b>{fixed(Math.abs(dDrift), 1)} bps</b>{' '}
        {dDrift <= 0 ? 'in the optimal schedule\u2019s favor' : 'against the optimal schedule'}. That part is
        luck, not skill; the {n}-window view below averages it away.
      </p>
      <p className="score__foot" style={{ ['--strat' as string]: META[focus].color }}>
        Click a row to focus the replay on that strategy.
      </p>
    </div>
  );
}

export const Scoreboard = memo(ScoreboardImpl);
