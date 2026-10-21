import type { ReactNode } from 'react';

export const REPO = 'https://github.com/ericniu0218/Optimal-execution';

export function Top() {
  return (
    <header className="top">
      <a className="brand" href="#top" aria-label="Optimal Execution Engine">
        <svg className="brand__mark" viewBox="0 0 32 32" aria-hidden="true">
          <rect x="1" y="1" width="30" height="30" rx="4" className="brand__box" />
          <path d="M5 8 C 12 8, 12 14, 16 16 S 20 24, 27 24" className="brand__curve" />
          <path d="M5 24 L27 8" className="brand__twap" />
        </svg>
        <span className="brand__name">Optimal Execution Engine</span>
      </a>
      <nav className="top__nav" aria-label="Project links">
        <a href="#tradeoff">The tradeoff</a>
        <a href="#verified">Verification</a>
        <a href={`${REPO}/blob/main/MODEL.md`}>Model</a>
        <a className="top__src" href={REPO}>
          Source
          <svg viewBox="0 0 16 16" aria-hidden="true">
            <path
              d="M4 12 12 4M5.5 4H12v6.5"
              fill="none"
              stroke="currentColor"
              strokeWidth="1.6"
              strokeLinecap="round"
              strokeLinejoin="round"
            />
          </svg>
        </a>
      </nav>
    </header>
  );
}

interface Stat {
  label: string;
  value: string;
  caption: ReactNode;
}

export function Hero({ stats }: { stats: Stat[] }) {
  return (
    <section className="hero" id="top">
      <div className="hero__copy">
        <p className="eyebrow">Almgren–Chriss execution · real NASDAQ order book · running in your browser</p>
        <h1 className="hero__title">
          <span className="hero__line">Sell fast and you move the price.</span>
          <span className="hero__line hero__line--ac">Sell slow and it moves you.</span>
        </h1>
        <p className="hero__sub">
          A trader has to sell a big block of AAPL. The best schedule hangs on one dial: how much risk they
          will carry to save on impact cost. Below, four strategies replay a real trading day against the
          actual order book. Turn the dial and the whole tradeoff recomputes.
        </p>
      </div>
      <ul className="stats" aria-label="Live results for the current setting">
        {stats.map((s) => (
          <li key={s.label} className="stat">
            <span className="stat__label">{s.label}</span>
            <span className="stat__value">{s.value}</span>
            <span className="stat__cap">{s.caption}</span>
          </li>
        ))}
      </ul>
    </section>
  );
}

export function Footer() {
  return (
    <footer className="foot">
      <p>
        Market data: LOBSTER sample files (AAPL, 2012-06-21), downsampled to one-second book snapshots for
        this demo. Built for education and research. Not investment advice.
      </p>
      <p>
        <a href={REPO}>Source on GitHub</a> · <a href={`${REPO}/blob/main/MODEL.md`}>MODEL.md</a> ·{' '}
        <a href={`${REPO}/blob/main/README.md`}>README</a>
      </p>
    </footer>
  );
}
