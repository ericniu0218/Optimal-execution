import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { loadDay, windowStarts, type DaySeries, type StrategyId } from './engine';
import { Dial } from './components/Dial';
import { Footer, Hero, Top } from './components/Chrome';
import { Order, Picker } from './components/Controls';
import { Heatmap } from './components/Heatmap';
import { PlaybackBar } from './components/PlaybackBar';
import { Scoreboard } from './components/Scoreboard';
import { Distribution, Frontier } from './components/Tradeoff';
import { Trajectories } from './components/Trajectories';
import { Verified } from './components/Verified';
import { fixed, int, signed } from './lib/format';
import { META, STRATEGIES } from './lib/strategies';
import { playback } from './model/playback';
import { riskReduction } from './model/running';
import {
  acDist,
  analyticAt,
  benchmarkDists,
  frontiers,
  lambdaOf,
  parentOf,
  simulateWindow,
  soldInFirstThird,
} from './model/simulate';

const DEFAULT_LOG_LAMBDA = Math.log10(3e-9);
/**
 * Default replay window: 12:45 ET (75 minutes after the open). It is the window whose
 * AC-versus-TWAP outcome sits at the MEDIAN of the 71 for the default order, chosen
 * deliberately so the first thing a visitor sees is typical, not flattering.
 */
const DEFAULT_START_MIN = 75;

const reducedMotion = (): boolean => window.matchMedia('(prefers-reduced-motion: reduce)').matches;

export function App() {
  const [day, setDay] = useState<DaySeries | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    loadDay()
      .then(setDay)
      .catch((e: unknown) => setError(e instanceof Error ? e.message : String(e)));
  }, []);

  if (error) {
    return (
      <div className="page">
        <Top />
        <div className="state" role="alert">
          <h1>The market data did not load.</h1>
          <p>
            {error}. Reload the page; if it keeps happening the data pack is missing from this deployment.
          </p>
        </div>
      </div>
    );
  }
  if (!day) {
    return (
      <div className="page">
        <Top />
        <div className="state" role="status">
          <div className="spin" aria-hidden="true" />
          <h1>Loading a real trading day…</h1>
          <p>23,400 one-second snapshots of the AAPL order book, 0.7 MB.</p>
        </div>
      </div>
    );
  }
  return <Demo day={day} />;
}

function Demo({ day }: { day: DaySeries }) {
  const [qty, setQty] = useState(10000);
  const [minutes, setMinutes] = useState(30);
  const [startMin, setStartMin] = useState(DEFAULT_START_MIN);
  const [logLambda, setLogLambda] = useState(DEFAULT_LOG_LAMBDA);
  const [focus, setFocus] = useState<StrategyId>('AC');
  const lambda = lambdaOf(logLambda);

  // ---- everything below is recomputed live from the engine ------------------------
  const cfg = { qty, minutes, startMin, logLambda };

  const bench = useMemo(() => benchmarkDists(day, qty, minutes), [day, qty, minutes]);
  const fr = useMemo(() => frontiers(day, qty, minutes), [day, qty, minutes]);

  const live = useMemo(() => {
    const t0 = performance.now();
    const ac = acDist(day, qty, minutes, lambda);
    const run = simulateWindow(day, cfg);
    return { ac, run, ms: performance.now() - t0 };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [day, qty, minutes, startMin, logLambda]);

  const { ac, run } = live;
  const parent = useMemo(() => parentOf({ qty, minutes, startMin }), [qty, minutes, startMin]);
  const model = useMemo(() => analyticAt(day, { qty, minutes }, logLambda), [day, qty, minutes, logLambda]);

  // ---- replay clock ------------------------------------------------------------------
  useEffect(() => {
    // `?end` opens the replay already finished: a shareable result, and the social-card capture.
    const finished = reducedMotion() || new URLSearchParams(window.location.search).has('end');
    playback.reset(minutes * 60, !finished);
    if (finished) playback.finish();
  }, [qty, minutes, startMin]);

  // ---- dial: continuous drag, plus animated jumps for the presets -----------------
  const raf = useRef(0);
  const cur = useRef(logLambda);
  cur.current = logLambda;
  const onDial = useCallback((v: number) => {
    cancelAnimationFrame(raf.current);
    setLogLambda(v);
  }, []);
  const onPreset = useCallback((target: number) => {
    cancelAnimationFrame(raf.current);
    if (reducedMotion()) return setLogLambda(target);
    const from = cur.current;
    const t0 = performance.now();
    const dur = 650;
    const step = (now: number) => {
      const p = Math.min(1, (now - t0) / dur);
      const e = p < 0.5 ? 4 * p * p * p : 1 - (-2 * p + 2) ** 3 / 2;
      setLogLambda(from + (target - from) * e);
      if (p < 1) raf.current = requestAnimationFrame(step);
    };
    raf.current = requestAnimationFrame(step);
  }, []);
  useEffect(() => () => cancelAnimationFrame(raf.current), []);

  const onMinutes = useCallback((m: number) => {
    setMinutes(m);
    // keep the replay start valid for the new window length
    const valid = windowStarts(m);
    setStartMin((s) =>
      valid.includes(s) ? s : valid.reduce((a, b) => (Math.abs(b - s) < Math.abs(a - s) ? b : a)),
    );
  }, []);

  // ---- derived display values ---------------------------------------------------------
  const twapDist = bench.TWAP;
  const riskDelta = ac.stdIs / twapDist.stdIs - 1;
  const impactDelta = ac.meanImpact - twapDist.meanImpact;
  const advantage = twapDist.isBps.map((v, i) => v - ac.isBps[i]!);
  const arrivalPrice = run.TWAP.arrivalMid / 1e4;

  const benchPoints = (['TWAP', 'VWAP', 'POV'] as const).map((id) => ({
    id,
    risk: bench[id].stdIs,
    cost: bench[id].meanImpact,
  }));

  const distRows = [
    { id: 'AC' as const, dist: ac },
    { id: 'TWAP' as const, dist: bench.TWAP },
    { id: 'VWAP' as const, dist: bench.VWAP },
    { id: 'POV' as const, dist: bench.POV },
  ];
  const domain = useMemo<[number, number]>(() => {
    const all = [...bench.TWAP.isBps, ...bench.VWAP.isBps, ...bench.POV.isBps, ...fr.isRange];
    const lo = Math.floor((Math.min(...all) - 2) / 5) * 5;
    const hi = Math.ceil((Math.max(...all) + 2) / 5) * 5;
    return [lo, hi];
  }, [bench, fr]);

  const stats = [
    {
      label: 'Risk vs TWAP',
      value: `${riskDelta <= 0 ? '−' : '+'}${fixed(Math.abs(riskDelta) * 100, 0)}%`,
      caption: (
        <>
          Std of cost over {ac.isBps.length} real windows: {fixed(twapDist.stdIs, 1)} →{' '}
          <b>{fixed(ac.stdIs, 1)}</b> bps
        </>
      ),
    },
    {
      label: 'Extra impact cost',
      value: `${signed(impactDelta, 2)} bps`,
      caption: (
        <>
          Mean cost, market drift removed: {fixed(twapDist.meanImpact, 2)} → <b>{fixed(ac.meanImpact, 2)}</b>{' '}
          bps
        </>
      ),
    },
    {
      label: 'Recomputed live',
      value: live.ms < 0.1 ? '<0.1 ms' : `${live.ms < 10 ? live.ms.toFixed(1) : Math.round(live.ms)} ms`,
      caption: <>All {ac.isBps.length} windows re-run in your browser each time the dial moves</>,
    },
  ];

  const sold = soldInFirstThird(run.AC);
  const exposureCut = riskReduction(run.AC, run.TWAP);

  return (
    <div className="page">
      <Top />
      <Hero stats={stats} />

      <main className="cockpit">
        <section className="card area-dial" aria-labelledby="h-dial">
          <header className="card__head">
            <h2 id="h-dial" className="card__title">
              Risk dial
            </h2>
            <span className="card__meta">drag, or use arrow keys</span>
          </header>
          <p className="card__lede">How much risk will you carry to save on impact cost?</p>
          <Dial logLambda={logLambda} onChange={onDial} onPreset={onPreset} />
          <dl className="readouts">
            <div>
              <dt>Sold in the first third</dt>
              <dd>
                {fixed(sold * 100, 0)}% <span>TWAP: 33%</span>
              </dd>
            </div>
            <div>
              <dt>Exposure to price moves</dt>
              <dd>
                {fixed((1 - exposureCut) * 100, 0)}% <span>of TWAP</span>
              </dd>
            </div>
          </dl>
        </section>

        <section className="card area-order" aria-labelledby="h-order">
          <header className="card__head">
            <h2 id="h-order" className="card__title">
              Order
            </h2>
            <span className="card__meta">
              {int(qty)} × ${fixed(arrivalPrice, 2)}
            </span>
          </header>
          <Order
            qty={qty}
            minutes={minutes}
            arrivalPrice={arrivalPrice}
            participation={run.TWAP.participation}
            onQty={setQty}
            onMinutes={onMinutes}
          />
        </section>

        <section className="card area-window" aria-labelledby="h-window">
          <header className="card__head">
            <h2 id="h-window" className="card__title">
              Replay window
            </h2>
            <span className="card__meta">{twapDist.startMin.length} real windows</span>
          </header>
          <Picker
            startMins={twapDist.startMin}
            advantage={advantage}
            selected={startMin}
            minutes={minutes}
            openSec={day.openSec}
            onPick={setStartMin}
          />
        </section>

        <section className="card area-replay" aria-labelledby="h-replay">
          <header className="card__head card__head--wrap">
            <h2 id="h-replay" className="card__title">
              The real book, replayed
            </h2>
            <div className="chips" role="group" aria-label="Focus strategy">
              {STRATEGIES.map((s) => (
                <button
                  key={s.id}
                  type="button"
                  className={`chip${focus === s.id ? ' chip--on' : ''}`}
                  aria-pressed={focus === s.id}
                  style={{ ['--strat' as string]: s.color }}
                  onClick={() => setFocus(s.id)}
                  title={s.blurb}
                >
                  <i /> {s.label}
                </button>
              ))}
            </div>
          </header>
          <Heatmap day={day} parent={parent} run={run} focus={focus} />
          <PlaybackBar />
          <p className="note">
            <span className="sw sw--bid" /> bids <span className="sw sw--ask" /> asks, brighter where more
            shares rest. Dashed line is the arrival price. Fills land <b>below</b> the real market because our
            own selling pushed it there; the shaded gap is{' '}
            <b style={{ color: META[focus].color }}>{META[focus].short}&apos;s permanent impact</b>.
          </p>
        </section>

        <section className="card area-traj" aria-labelledby="h-traj">
          <header className="card__head">
            <h2 id="h-traj" className="card__title">
              Shares remaining
            </h2>
            <span className="card__meta">the dial reshapes the blue line</span>
          </header>
          <Trajectories run={run} focus={focus} minutes={minutes} />
        </section>

        <section className="card area-score" aria-labelledby="h-score">
          <header className="card__head">
            <h2 id="h-score" className="card__title">
              Cost vs. arrival price
            </h2>
            <span className="card__meta">bps · lower is better</span>
          </header>
          <Scoreboard run={run} focus={focus} windows={ac.isBps.length} onFocus={setFocus} />
        </section>
      </main>

      <section className="tradeoff" id="tradeoff" aria-labelledby="h-trade">
        <header className="section__head">
          <h2 id="h-trade" className="section__title">
            One dial, one tradeoff
          </h2>
          <p className="section__lede">
            Turning the dial toward urgent buys certainty and pays for it. The left chart is the model&apos;s
            prediction next to what {ac.isBps.length} real windows delivered; the right chart is the same{' '}
            {ac.isBps.length} windows, dot by dot. Drag the dial and both move.
          </p>
        </header>
        <div className="tradeoff__grid">
          <section className="card" aria-labelledby="h-front">
            <header className="card__head">
              <h3 id="h-front" className="card__title">
                Efficient frontier
              </h3>
              <span className="card__meta">model vs. reality</span>
            </header>
            <Frontier
              analytic={fr.analytic}
              realized={fr.realized}
              bench={benchPoints}
              ac={{ risk: ac.stdIs, cost: ac.meanImpact }}
              model={{ risk: model.risk, cost: model.cost }}
              logLambda={logLambda}
            />
          </section>
          <section className="card" aria-labelledby="h-dist">
            <header className="card__head">
              <h3 id="h-dist" className="card__title">
                Every window, every strategy
              </h3>
              <span className="card__meta">spread shrinks as the dial turns</span>
            </header>
            <Distribution rows={distRows} domain={domain} selected={startMin} minutes={minutes} />
          </section>
        </div>
      </section>

      <Verified />
      <Footer />
    </div>
  );
}
