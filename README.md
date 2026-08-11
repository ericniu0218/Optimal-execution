# Optimal Execution Engine (Almgren-Chriss)

If you need to sell N shares over T minutes, trading fast consumes liquidity
(market impact) while trading slow leaves you exposed to price drift (timing
risk). This project implements the Almgren-Chriss (2000) optimal execution
framework end-to-end: parse real limit-order-book data (LOBSTER), calibrate
impact parameters from it, compute the closed-form optimal trading trajectory,
and backtest it against TWAP / VWAP / POV benchmarks by replaying the real
book, measuring implementation shortfall for each.

Core engine in C++20; calibration and plotting in Python.

## Model

Liquidate X shares over [0, T] in N slices. With linear permanent impact
(each trade shifts the "true" price by γ per share) and linear temporary
impact (trading at rate v costs η·v per share on top of the half-spread ε),
minimizing `E[cost] + λ·Var[cost]` gives the discrete first-order condition

    (x_{j-1} - 2x_j + x_{j+1}) / τ² = κ̃² x_j ,   κ̃² = λσ²/η̃ ,   η̃ = η - γτ/2

with the exact discrete solution

    x_j = X · sinh(κ(T - t_j)) / sinh(κT) ,   κ = (2/τ)·asinh(κ̃τ/2)

λ = 0 recovers TWAP exactly; large λ front-loads execution. The solver uses
the exact discrete recursion (not the continuous-time approximation — a flag
exposes the difference) and is validated against an independent tridiagonal QP
solve of the same objective. Numerics: sinh ratios are evaluated in an
expm1-based form stable at both κT → 0 and κT ≫ 1.

## Layout

    cpp/include/oee/core/    value types: integer tick prices, ns timestamps
    cpp/include/oee/data/    LOBSTER parser, SoA tapes, trade-event aggregation
    cpp/include/oee/ac/      Almgren-Chriss closed-form solver + QP reference
    cpp/src/, cpp/tests/     implementations, GoogleTest suites
    python/                  calibration + plotting (later phase)
    data/raw/                LOBSTER CSVs (gitignored; see data/README.md)

Data-handling details that matter for correctness:

- **Prices are int64 ticks** (1e-4 dollars, LOBSTER's native lattice);
  doubles appear only at the analysis boundary.
- **Sweep aggregation**: a marketable order matching K resting orders appears
  in LOBSTER as K same-timestamp rows; these are regrouped into single trade
  events before any impact estimation.
- **Hidden executions (type 5)** carry unreliable direction; they are
  classified by the quote rule / tick test (Lee-Ready) against the pre-event
  book, kept out of the visible-book replay, and included/excluded from
  calibration explicitly.
- **Sentinel book levels** (±9999999999 padding) are normalized at parse time.

## Limitation: the backtest is not a counterfactual

Historical order book states are replayed from LOBSTER as they occurred, in a
world where this strategy's orders never existed. The simulator models the
strategy's own impact in reduced form — child orders consume visible depth
within each event, and accumulated permanent impact shifts the entire replayed
ladder for all subsequent events. It does not model how real participants
would have *reacted* to this order flow: no cancellation, requoting, or
liquidity-provision response is simulated. A message-driven reconstruction
that inserts synthetic orders into the live book would fix depth accounting
but not this, since the subsequent message stream remains the un-impacted one.
Reported implementation shortfall should therefore be read as a lower bound on
true cost, and comparisons *between* strategies (all subject to the same bias)
are more trustworthy than absolute levels.

## Build & test

Requires CMake ≥ 3.20 and a C++20 compiler (GoogleTest is fetched
automatically):

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j
    ctest --test-dir build --output-on-failure

## Status

- [x] LOBSTER message/orderbook parser (SoA tapes, sentinel normalization)
- [x] Trade-event aggregation (sweeps, hidden-order classification)
- [x] Almgren-Chriss closed-form solver, validated against QP reference
- [x] Execution simulator: depth-walk fills + pluggable impact model
      (permanent impact shifts the whole replayed ladder; temporary impact
      is emergent from spread crossing + depth consumption)
- [x] Strategies: TWAP, VWAP (parametric U-curve), POV, Almgren-Chriss —
      the static schedules share one ScheduledStrategy engine
- [x] Backtester: shared slice grid, market-volume feed, Perold
      implementation shortfall with exact drift/permanent/execution/
      opportunity decomposition; `oee_backtest` compares all four
      strategies on a real day and writes results CSVs
- [x] Impact calibration from LOBSTER: σ (realized vol), γ (mid change on
      net signed flow, R² 0.28–0.56 across tickers), η via the mechanical
      depth-walk cost curve (R² ≥ 0.97, intercept reproduces the quoted
      half-spread). The naive event-slippage regression is kept as a
      documented negative result — its slope is ~0 or negative because
      aggressors size orders to available liquidity (selection bias).
      `oee_dump` exports; `python/calibrate.py` fits and emits JSON.
- [x] Analysis layer: `oee_frontier` (analytic E-vs-sqrt(V) sweep),
      `python/run_sweep.py` (realized IS per λ), `python/plot_results.py`
      (trajectories, efficient frontier, IS-vs-λ figures)

Pass 1 is complete end-to-end. Pass 2 in progress:

- [x] Multi-window cost distributions: `oee_windows` runs every strategy
      over rolling 30-min windows (71 per day, ~500 backtests in 0.2s) and
      `plot_distributions.py` renders the distributional comparison.
- [x] Message-driven book rebuild, validated row-for-row against LOBSTER's
      own snapshots (`oee_rebuild`, ~3M rows/s)
- [x] Binary tape cache (`<message>.csv.oeb`, auto-populated); GitHub
      Actions CI across Linux/macOS x Release/Debug, plus ASan and UBSan
      jobs (`-DOEE_SANITIZE=address,undefined`)
- [x] Obizhaeva-Wang transient impact with exponential resilience
      (`TransientImpact`, `--kappa`/`--rho`), plus the empirical decay
      measurement that tests whether it is needed
- [x] Adaptive AC with a drift term (`AdaptiveAcStrategy`), including the
      closed-form drift solution and its own QP validation

Pass 2 complete.

### Binary tape cache

Every tool loads through `load_day_cached`, which parses the CSVs once and
thereafter reads a direct image of the in-memory SoA tapes. End-to-end tool
runtime, full day:

| ticker | CSV | cache | cold | warm |
|---|---|---|---|---|
| GOOG | 38 MB | 49 MB | 0.39 s | 0.02 s |
| AAPL | 105 MB | 135 MB | 0.26 s | 0.04 s |
| INTC | 175 MB | 210 MB | 0.31 s | 0.04 s |

The cache is deliberately ~30% *larger* than the CSV (prices and sizes are
narrow as text, 8 bytes in memory) and deliberately non-portable: it is a
derived artifact keyed to the source files' byte sizes, rejected rather
than misread when stale, truncated, or written by a different layout. That
trade — disk for parse time — is the whole point, and it is what makes the
71-window x 7-strategy sweep a sub-second operation.

### Adaptive execution — why the obvious version is vacuous

The natural "adaptive AC" — re-solve the same problem each slice on the
remaining shares — adds nothing: under mean-variance the re-solve is
time-inconsistent, and under CARA with Gaussian prices the optimal
strategy is provably *deterministic* (Schied & Schöneborn 2009). It would
look adaptive while reproducing the static trajectory exactly. Real
adaptivity needs information the static solve lacked, so this implements
the **drift** extension: α is re-estimated from the realized mid path and
the remaining trajectory re-solved with it. Selling into a rising market,
holding pays and the schedule slows; falling, it accelerates.

With drift the first-order condition keeps the same linear recursion plus
a constant forcing term, so the closed form gains a constant:

    x_j = x̄ + u·e^(−κt_j) − (x̄ + u·e^(−κT))·sinh(κt_j)/sinh(κT),
    x̄ = α/(2λσ²),  u = X − x̄

and is held to the same bar as the drift-free solver: agreement with an
independent tridiagonal QP to 1e-11 across α ∈ [−0.05, 0.05].

Three results, in the order they were found:

1. **Uncapped, it is catastrophic.** α enters through x̄ = α/(2λσ²), and
   that denominator is tiny at realistic λ — a noisy slope estimate
   produces a target inventory larger than the entire order, deferring
   everything to the deadline. Measured: std of IS **10 → 342 bps**. The
   fix is a cap that bounds |x̄| by a fraction of shares remaining, which
   is the load-bearing safety parameter, not a tuning knob.
2. **Capped, it is neutral.** Across the same 71 windows, adaptive AC
   lands within ±0.05 bps of its static twin on both mean cost and risk at
   every cap level tested (0.1/0.25/0.5) — indistinguishable from noise.
3. **And here is why.** Regressing each window's realized drift over its
   remaining 20 minutes on the drift over its first 10 minutes gives
   **corr = −0.10, R² = 0.009**. Short-horizon drift simply does not
   predict remaining-horizon drift on this data, so there is nothing for
   an adaptive schedule to harvest — the machinery can only add tail risk.

The honest conclusion is that adaptive execution is a bet on drift
predictability, and this data does not support the bet. That is a more
useful finding than a tuned improvement would have been, and it is why
the headline results use the static solver.

### Impact resilience — measured, then stress-tested

`TransientImpact` implements Obizhaeva-Wang impact,
`shift(t) = γ·Q(t) + κ·Σ qᵢ·e^(−ρ(t−tᵢ))`, which **nests everything the
project assumed before it**: ρ→0 is pure permanent impact, ρ→∞ is the
instant-book-recovery assumption Pass 1 backtests ran on. So the question
"was Pass 1's assumption defensible?" became measurable rather than
rhetorical. `oee_dump` emits three curves per ticker (`decay.csv`):

1. **Unconditional mid response rises with horizon** (AAPL: 146 → 524
   ticks over 10s) — it does *not* decay. That is order-flow
   autocorrelation, not absent resilience: a trade is usually one slice of
   somebody's metaorder, so correlated flow follows and keeps pushing the
   mid. This is precisely why propagator models exist (Bouchaud et al.).
2. **Conditioning on isolated trades** (no other trade within the horizon)
   removes most of it — but the response still does not mean-revert.
3. **Aggregate visible depth on the aggressed side is already back to
   ~1.00-1.09× its pre-trade level in the first post-trade snapshot** and
   stays flat thereafter, on all three tickers.

Conclusion: for typical trade sizes against 10-level depth, the book heals
faster than this data can resolve, so **ρ→∞ — Pass 1's implicit
assumption — is empirically defensible**, and no transient term is fitted
into the headline results. The model ships anyway as a sensitivity knob,
because "we measured it and it did not matter here" is only credible if
you can show what would change if it did. Splitting γ evenly into a
transient half (AAPL, 71 windows, mean impact cost in bps):

| resilience | TWAP | AC λ=3e-9 | AC λ=3e-8 | fast-minus-TWAP penalty |
|---|---|---|---|---|
| permanent only (ρ→0) | 5.39 | 5.96 | 6.35 | 0.96 |
| 1 s half-life | 3.65 | 4.25 | 4.84 | 1.19 |
| 30 s half-life | 3.68 | 4.32 | 4.98 | **1.30** |

The mechanism shows up exactly where theory says it should: slower
recovery penalizes *fast* strategies most, because front-loaded schedules
place their child orders close enough together to trade into their own
un-healed impact. Resilience steepens the cost arm of the AC tradeoff
without touching the risk arm (std of IS is unchanged to 0.02 bps).

### Book reconstruction (`oee_rebuild`)

The visible book is rebuilt from the message stream alone and compared
against every one of the day's snapshots. A level-N LOBSTER file omits
events outside the top N levels, which makes two things unobservable by
format: liquidity that scrolls INTO view was never announced, and levels
that scroll OUT of view go stale invisibly (this second one was found by
debugging real AAPL data — the rebuild now prunes levels that leave the
window). Every row is classified exact / surfaced (format-inherent,
reseeded and counted) / hard (the rebuild claims liquidity the exchange
denies — a genuine bug). Full-day results, 2012-06-21:

| | rows | exact | surfaced | hard |
|---|---|---|---|---|
| AAPL | 400,390 | 73.9% | 26.1% | **0** |
| INTC | 624,039 | 99.7% | 0.3% | **0** |
| GOOG | 147,915 | 69.9% | 30.1% | **0** |

Zero hard mismatches across 1.17M rows: every row is either reproduced
exactly or explained by the format's visibility boundary, never
contradicted. The exact-rate spread is itself informative: INTC's dense
penny book keeps the same 10 price levels on screen nearly all day, while
AAPL/GOOG's sparse wide books churn the visibility boundary constantly.

### The headline result (AAPL, 10k shares / 30 min, 71 rolling windows)

| strategy | mean IS (bps) | std IS (bps) | mean impact = IS − drift (bps) |
|---|---|---|---|
| TWAP | +10.2 | 11.5 | 5.39 |
| VWAP | +10.3 | 11.0 | 5.45 |
| POV(5%) | +11.2 | 13.3 | 5.60 |
| AC λ=3e-11 | +10.1 | 11.4 | 5.39 |
| AC λ=3e-10 | +9.4 | 10.2 | 5.44 |
| AC λ=3e-9 | +7.6 | 6.9 | 5.96 |
| AC λ=3e-8 | +6.9 | 5.8 | 6.35 |

Exactly the Almgren-Chriss tradeoff, realized on real data: as λ rises,
timing risk falls monotonically (11.4 → 5.8 bps) while impact cost rises
monotonically (5.39 → 6.35 bps); AC at λ→0 is indistinguishable from TWAP
on both axes; and POV is inside the frontier (dominated: more risk AND
more cost). The realized (risk, impact-cost) points track the analytic
frontier within ~0.5 bps at every λ
(`results/AAPL/figs/realized_frontier.png`). Raw mean IS *falls* with λ
on this day only because 2012-06-21 trended down all session — a seller
was paid for speed; the drift-removed column is the model-comparable one.
Caveats: windows overlap (serially correlated samples) and this is one
trading day.

Calibration findings on the 2012-06-21 samples (see `results/dump/*/calib.json`):

| | AAPL | INTC | GOOG |
|---|---|---|---|
| γ (ticks/share) | 0.42 | 0.0079 | 1.59 |
| k = ∂cost/∂q (ticks/share) | 1.14 | 0.0016 | 2.39 |
| ε fitted vs quoted (ticks) | 748 / 750 | 49.8 / 50 | 1323 / 1250 |

Three findings worth calling out: (1) temporary-impact slopes span three
orders of magnitude between INTC and GOOG — the liquidity contrast is the
dominant fact of cross-ticker execution; (2) INTC violates the AC
well-posedness condition η̃ = τ(k − γ/2) > 0 at *every* slicing, because
the flow-regression γ (which prices in the information content of market
order flow) exceeds the mechanical cost slope on a tick-constrained name.
The backtester reports this and runs the remaining strategies; using
flow-γ for uninformed liquidation is a known overstatement (cf. Almgren
et al. 2005 calibrating on proprietary uninformed orders instead).
(3) In the λ-sweep, realized IS saturates (~22 bps on the AAPL demo order)
once κT is large enough that the schedule is effectively "everything now":
visible book depth physically caps the execution rate via partial fills,
while the analytic E[IS] keeps rising on the linear model's extrapolation.
The gap between the two curves is the linear-impact assumption made
visible (`results/AAPL/figs/is_vs_lambda.png`).

## Reproducing the results

```
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build
python3 -m venv .venv && .venv/bin/pip install matplotlib
# data/raw/: see data/README.md, then per ticker:
./build/oee_dump --msg <msg.csv> --book <book.csv> --out-dir results/dump/AAPL
python3 python/calibrate.py --dump-dir results/dump/AAPL --ticker AAPL
.venv/bin/python python/run_sweep.py --calib results/dump/AAPL/calib.json \
    --ticker AAPL --msg <msg.csv> --book <book.csv> --qty 20000
.venv/bin/python python/plot_results.py --dir results/AAPL --ticker AAPL
./build/oee_windows --msg <msg.csv> --book <book.csv> --qty 10000 \
    --gamma 0.421774 --eta 1.13567 --sigma 2694.48 --epsilon 750 \
    --out results/AAPL/windows.csv
./build/oee_frontier --qty 10000 --duration-min 30 --slices 30 \
    --gamma 0.421774 --eta 1.13567 --sigma 2694.48 --epsilon 750 \
    --arrival-mid 5850000 --lambda-min 3e-11 --lambda-max 3e-8 \
    --points 40 --out results/AAPL/frontier_windows.csv
.venv/bin/python python/plot_distributions.py --dir results/AAPL --ticker AAPL
```

## References

- Almgren, R. and N. Chriss (2000). *Optimal Execution of Portfolio
  Transactions.* Journal of Risk 3, 5-39.
- Lee, C. and M. Ready (1991). *Inferring Trade Direction from Intraday
  Data.* Journal of Finance 46(2).
- LOBSTER: lobsterdata.com — academic limit order book data.
