# Live demo

A browser demo of the Almgren-Chriss execution engine. It replays a real NASDAQ order book (AAPL, 2012-06-21)
and lets a visitor turn a risk-aversion dial while four strategies (TWAP, VWAP, POV, and the optimal Almgren-Chriss
schedule) recompute across 71 rolling windows of that day. Nothing runs on a server: the whole engine executes in
the visitor's browser, so it deploys as a static site.

## Run it locally

```bash
cd web
npm install
npm run dev        # http://localhost:5173
npm test           # engine verification against the C++ output
npm run build      # typecheck + production build into web/dist
```

Requires Node 20.19+ or 22.12+.

## Deploy to Vercel

The app is a static Vite build. The only setting that matters is the **Root Directory**, because the site lives in
`web/` and not at the repository root.

**From the dashboard**

1. vercel.com, Add New, Project, import the GitHub repository.
2. Set **Root Directory** to `web`. Vercel detects Vite on its own (build `npm run build`, output `dist`).
3. Deploy.

**From the terminal**

```bash
npm i -g vercel
cd web
vercel            # first deploy creates a preview; answer the prompts
vercel --prod     # promote to production
```

The social-share image needs an absolute URL. On Vercel the build reads `VERCEL_PROJECT_PRODUCTION_URL`
automatically. If you attach a custom domain, set an environment variable `SITE_URL=https://your.domain` in the
project settings and redeploy.

## How it works

| Piece       | Where             | Notes                                                                                           |
| ----------- | ----------------- | ----------------------------------------------------------------------------------------------- |
| Engine      | `src/engine/`     | TypeScript port of the C++ solver, simulator, strategies, and backtester.                       |
| Market data | `public/data/`    | 23,400 one-second snapshots of the AAPL book (ten levels a side), about 0.7 MB.                 |
| Live model  | `src/model/`      | Turns one dial position into the replayed window, the 71-window distribution, and the frontier. |
| Interface   | `src/components/` | Canvas heatmap and trajectories, SVG dial and charts. No chart library.                         |

A dial turn re-runs every window in roughly 1 to 5 ms, which is why everything on the page updates live and nothing
is pre-rendered.

### Why it can be trusted

`npm test` holds the TypeScript engine to the C++ engine's recorded output (`src/engine/golden.json`):

- every child order's size, fill, ladder shift, and pre-trade mid in the reference window match **integer for integer**;
- all 71 windows across 7 strategies agree to within 0.002 bps;
- the four cost terms (drift, permanent impact, execution, opportunity) sum to total shortfall on every window;
- the closed-form schedule matches an independent tridiagonal solve of the same objective.

The tests were mutation-checked: breaking the cost decomposition, or swapping C++ `llround` for JavaScript's
`Math.round` in the impact shift, makes them fail.

## The market data

`public/data/aapl-2012-06-21.v1.pack` is derived from the free **LOBSTER** sample files. It is a 1-second
downsample of the top ten book levels, plus per-second executed volume. It is not the original message stream.

LOBSTER is an academic data service and its sample files are offered for research and teaching. Confirm its terms
of use before redistributing derived data publicly. To ship without the market data, delete `public/data/`; the demo
then shows its "market data did not load" state. A synthetic book can be swapped in by producing a pack in the same
format (see the header of `scripts/build-data.py` for the layout).

## Regenerating the data and fixtures

Both scripts read the raw LOBSTER files and the C++ results, which are git-ignored.

```bash
# from the repository root
python3 -I web/scripts/build-data.py \
  data/raw/AAPL_2012-06-21_34200000_57600000_message_10.csv \
  data/raw/AAPL_2012-06-21_34200000_57600000_orderbook_10.csv \
  web/public/data AAPL 2012-06-21

python3 -I web/scripts/build-golden.py results/AAPL web/src/engine/golden.json
```

## Known limits of the demo

- AAPL only. The INTC calibration violates the model's own well-posedness condition, and the engine refuses it
  rather than return a nonsense schedule.
- Order sizes and durations are restricted to values where every slice is exactly one minute, so slice times land
  on whole seconds and the browser sees byte-identical books to the C++ backtester.
- Everything in the root `README.md` under "Honest limitations" applies here too: this is a replay, not a
  counterfactual.
