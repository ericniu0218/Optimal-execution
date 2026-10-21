const CHECKS = [
  {
    big: 'Integer for integer',
    title: 'Matches the C++ engine exactly',
    body: 'This page runs a TypeScript port of the C++ backtester. In the reference window every child order, fill, and price impact matches the C++ output integer for integer, and all 71 windows across 7 strategies agree to within 0.002 bps. npm test re-checks it.',
  },
  {
    big: '1.17M rows, 0 contradictions',
    title: 'A real order book, rebuilt from scratch',
    body: 'The C++ side rebuilds the book from the raw message feed alone and compares it with the exchange’s own snapshots across three tickers. Every row either matches or is explained by what the data format hides. None contradict.',
  },
  {
    big: '1 part in 10¹¹',
    title: 'The optimal schedule is cross-checked',
    body: 'The closed-form trajectory is verified against an independent tridiagonal solve of the same optimization. Different algorithm, no shared code.',
  },
  {
    big: '92 tests',
    title: 'Sanitized, on real data',
    body: 'The C++ suite passes CI on Linux and macOS, including AddressSanitizer and UBSan jobs. The tools were also run on the real market data under both sanitizers.',
  },
];

const CALIB = [
  { t: 'AAPL', g: '0.42', k: '1.14', e: '748', note: 'This demo' },
  { t: 'GOOG', g: '1.59', k: '2.39', e: '1,323', note: 'Thin, wide book' },
  { t: 'INTC', g: '0.0079', k: '0.0016', e: '49.8', note: 'Model refuses (see below)' },
];

const LIMITS = [
  {
    h: 'It is a replay, not a counterfactual.',
    p: 'History is replayed as it happened, in a world where these orders never existed. The simulator charges our own trades for the liquidity they eat and shifts the book for our later orders, but other traders never react. Read costs as a lower bound, and trust comparisons between strategies over any single absolute number.',
  },
  {
    h: 'One trading day per stock.',
    p: 'The 71 windows overlap, so they are correlated rather than independent samples. This day trended down, which flatters faster selling in raw shortfall. That is why the charts show impact cost with the market’s drift removed.',
  },
  {
    h: 'Marketable orders only.',
    p: 'No resting orders and no queue position. Modelling those honestly means modelling adverse selection, a different and harder problem than the one this project scopes to.',
  },
  {
    h: 'It does not work for every stock.',
    p: 'For INTC the fitted parameters violate a condition the model needs to be well posed. The engine detects that and refuses instead of returning a nonsense schedule, which is why this demo runs on AAPL.',
  },
];

export function Verified() {
  return (
    <section className="verified" id="verified" aria-labelledby="h-verified">
      <header className="section__head">
        <h2 id="h-verified" className="section__title">
          Why you can believe the numbers
        </h2>
        <p className="section__lede">
          A demo that only looks right is easy to build. This one is held to the C++ engine it was ported
          from.
        </p>
      </header>

      <ul className="checks">
        {CHECKS.map((c) => (
          <li key={c.title} className="check">
            <span className="check__big">{c.big}</span>
            <h3 className="check__title">{c.title}</h3>
            <p className="check__body">{c.body}</p>
          </li>
        ))}
      </ul>

      <div className="split">
        <div className="calib">
          <h3 className="mini">Fitted from the real data</h3>
          <table>
            <thead>
              <tr>
                <th scope="col">Ticker</th>
                <th scope="col" title="Permanent impact, ticks of price per share sold">
                  γ
                </th>
                <th scope="col" title="Cost slope of walking the book, ticks per share">
                  k
                </th>
                <th scope="col" title="Fitted half-spread, ticks">
                  ε
                </th>
                <th scope="col" className="calib__note-h">
                  Note
                </th>
              </tr>
            </thead>
            <tbody>
              {CALIB.map((r) => (
                <tr key={r.t}>
                  <th scope="row">{r.t}</th>
                  <td>{r.g}</td>
                  <td>{r.k}</td>
                  <td>{r.e}</td>
                  <td className="calib__note">{r.note}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <p className="calib__foot">
            Temporary impact differs about 1,500× between INTC and GOOG. The slope k comes from walking the
            historical book directly; the obvious regression on trade size fits nothing (R² ≈ 0) because
            traders size orders to the liquidity available.
          </p>
        </div>

        <div className="limits">
          <h3 className="mini">What this does not do</h3>
          <ul>
            {LIMITS.map((l) => (
              <li key={l.h}>
                <b>{l.h}</b> {l.p}
              </li>
            ))}
          </ul>
        </div>
      </div>
    </section>
  );
}
