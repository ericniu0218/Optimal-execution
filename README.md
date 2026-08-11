# Optimal Execution Engine

If you need to sell a large block of stock, you have a problem that has
nothing to do with picking the right stock: *how* you sell it changes what
you get paid. Dump it all on the market at once and you push the price down
against yourself before you're done selling — like trying to unload a truck
of furniture at one yard sale, the price drops with every piece once buyers
sense how much is coming. Sell it off slowly and gradually instead, and
you're safe from that effect, but now you're exposed for hours to a price
that moves around for reasons that have nothing to do with you. This project
builds and tests, on real stock exchange data, the classic model
(Almgren-Chriss, 2000) for finding the best point between those two extremes.

## Why this is hard

This isn't a toy problem — every trading desk that executes large orders has
people and systems dedicated to exactly this question, because it's real
money either way: trade too fast and you're paying a visible, measurable toll
in market impact; trade too slow and you're gambling on an invisible one
(price drift) that you can't control. The two costs don't share a unit you
can just add up and minimize — one is a near-certain cost you cause
yourself, the other is a risk you're exposed to — so there's no single
"correct answer," only a tradeoff curve and a choice of how much risk you're
willing to carry to save on cost. Getting that tradeoff right, and proving
it holds on real market data instead of just on paper, is the actual
engineering problem here.

## Results

Real 2012 NASDAQ order-by-order data (LOBSTER) for AAPL, replayed through a
simulator that walks the actual historical order book. Four strategies —
TWAP (split evenly over time), VWAP (weight by typical volume), POV (trade
proportional to whatever everyone else is trading), and the Almgren-Chriss
optimal schedule — tested across 71 independent windows of the trading day.

![Execution trajectories by strategy](docs/figs/trajectories.png)
*How fast each strategy sells off the position.*

![Cost distribution by strategy across 71 windows](docs/figs/distributions.png)
*Each dot is one backtest — the optimal strategy's spread of outcomes
visibly tightens as it's tuned more risk-averse.*

![Realized vs. predicted efficient frontier](docs/figs/realized_frontier.png)
*The efficient frontier — the lowest cost achievable at each risk level —
predicted by the model (light curve) vs. what actually happened on real
trades (dark points).*

**Headline numbers** (cost in basis points, bps — 1 bp = 0.01% of trade
value):

- **Cut execution risk roughly in half** (11.5 → 5.8 bps standard deviation
  of cost, i.e. how much the outcome bounces around) for **well under 1
  extra bp of average cost** (5.39 → 6.35 bps), by trading faster.
- The naive POV strategy was **worse on both cost and risk at once**
  (5.60 bps, 13.3 bps) than the optimal schedule — strictly dominated.
- Realized cost matched the model's own prediction to **within 0.15 bps**
  across most of the range tested — except at the single most aggressive
  setting, where it wants to trade faster than the visible order book has
  liquidity to absorb (a real constraint the idealized math doesn't
  capture), opening a ~2 bp gap.

## How it works

Picture the tradeoff as a dial. Turn it toward "patient" and the schedule
spreads your selling evenly across the whole time window — safest from
self-inflicted price impact, but you're exposed to random price drift the
entire time. Turn it toward "urgent" and the schedule front-loads most of
the selling into the first few minutes — you get the uncertainty off your
plate fast, but you pay more because you're demanding liquidity faster than
the market naturally wants to supply it. The Almgren-Chriss model turns that
dial into math: given how volatile the stock is, how expensive it is to
trade it quickly (some stocks have deep, forgiving order books; others are
thin and punish size), and where you personally want the dial set, it
outputs the mathematically optimal minute-by-minute selling schedule.
Setting the dial to "totally patient" recovers the naive evenly-spread
strategy exactly, which is a nice sanity check that the math is doing what
it claims — the more sophisticated strategy contains the simple one as a
special case, not as a completely different formula.

The full derivation — the cost/risk formulas, the closed-form solution, the
drift and resilience extensions, and the calibration methodology — is in
[MODEL.md](MODEL.md) for anyone who wants to check the math rather than
trust the summary.

## Engineering highlights

- **C++20 core, Python analysis layer.** All performance- and
  correctness-critical logic (parsing, book replay, execution simulation,
  the solver) is C++; calibration and plotting are Python. The two never
  duplicate logic — Python only does regression over CSVs a tested C++ tool
  already produced, so there is exactly one implementation of anything
  trap-prone.
- **Built against real exchange data, not synthetic fixtures.** LOBSTER's
  nanosecond-timestamped message-by-message feed is unforgiving: multi-level
  sweeps arrive as several same-timestamp rows that have to be regrouped
  into one trade, hidden-order executions carry unreliable direction and
  need to be classified from the surrounding quotes, and this project found
  in real AAPL data (not by inspection) that a book price level that
  scrolls out of the top-10-levels-visible window and back in can look like
  phantom liquidity if you don't explicitly account for the format's own
  blind spot. All of these are handled and covered by tests that fail
  without the fix, not just described.
- **Independently verified, not just run once.** The optimal-schedule solver
  is checked against an independently-coded numerical solve of the same
  problem (different algorithm, shares no code), agreeing to 1 part in 10^11.
  The order book reconstruction — rebuilt from scratch using only the raw
  event stream — was checked row-by-row against the exchange's own recorded
  book state across all 1.17M rows in the three test days: zero
  contradictions.
- **Two independent estimators for the same parameter, on purpose, and one
  is reported as a documented failure.** The obvious way to estimate
  temporary market-impact cost (regress trade cost on trade size) turns out
  to be nearly uncorrelated with anything (R² ≈ 0) on this data, because
  traders size their orders to the liquidity available rather than the
  other way around. The estimator actually used instead — replaying the
  historical book to directly measure the cost of consuming a given size —
  fits far better (R² ≥ 0.97) and independently reproduces the exchange's
  quoted bid-ask spread as a side effect. Both are in the codebase and the
  failure is explained, not hidden.
- **A model checking its own assumptions.** The backtester assumes (as
  Almgren-Chriss does) that the order book refills instantly between
  trades. Rather than leave that as an unstated assumption, the project
  measured actual post-trade book depth recovery from the data and built a
  second impact model (with a decay rate) to quantify what would change if
  that assumption were wrong. It turned out to be a reasonable assumption
  at these trade sizes — but that's a measured conclusion, not an assumed
  one.
- **92 automated tests**, GitHub Actions CI across Linux/macOS in both
  Release and Debug, plus dedicated AddressSanitizer and
  UndefinedBehaviorSanitizer jobs that run the full suite and the CLI tools
  against real market data.

## Honest limitations

- **The backtest is not a full counterfactual.** Historical order book
  states are replayed as they actually occurred, in a world where this
  project's simulated orders never existed. The simulator charges the
  strategy's own trades for the liquidity they consume and shifts the book
  for the strategy's *own* future orders, but it does not simulate other
  real participants reacting to that order flow — no cancellations, no
  requoting. This makes reported costs a reasonable lower bound and makes
  *comparisons between strategies* (all subject to the same bias) more
  trustworthy than any single absolute number.
- **One trading day per stock**, not a multi-day history. The 71-window
  result is 71 overlapping (and therefore correlated, not independent)
  samples from a single day, and results for a strongly trending day will
  differ from a flat one — this is disclosed directly next to the results,
  not buried.
- **No modeling of resting/passive orders or queue position.** Every
  simulated order is a marketable order that crosses the spread
  immediately. This matches what the underlying model actually assumes
  (a trading *rate*, not a probability of getting filled while waiting in
  line), and modeling passive orders honestly would mean modeling adverse
  selection, which is a different, harder problem than the one this project
  scopes to.
- **The optimal schedule isn't well-defined for every stock.** For one of
  the three tickers tested (INTC), the calibrated parameters violate the
  model's own mathematical precondition — this is a real, disclosed finding
  (see MODEL.md), not a bug, and the code detects and reports it rather than
  silently returning a nonsensical answer.

## How to run it

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build --output-on-failure     # 92 tests

# Get sample data (see data/README.md), then per ticker:
./build/oee_dump --msg <message.csv> --book <orderbook.csv> --out-dir results/dump/AAPL
python3 python/calibrate.py --dump-dir results/dump/AAPL --ticker AAPL

python3 -m venv .venv && .venv/bin/pip install matplotlib
.venv/bin/python python/run_sweep.py --calib results/dump/AAPL/calib.json \
    --ticker AAPL --msg <message.csv> --book <orderbook.csv> --qty 20000
.venv/bin/python python/plot_results.py --dir results/AAPL --ticker AAPL

./build/oee_windows --msg <message.csv> --book <orderbook.csv> --qty 10000 \
    --gamma 0.421774 --eta 1.13567 --sigma 2694.48 --epsilon 750 \
    --out results/AAPL/windows.csv
.venv/bin/python python/plot_distributions.py --dir results/AAPL --ticker AAPL

./build/oee_rebuild --msg <message.csv> --book <orderbook.csv>   # book validation
```

## What I'd extend with more time

- **Multiple trading days** per ticker, to see whether the single-day
  findings here (especially the resilience measurement, which was
  inconclusive partly from limited data) hold up or shift.
- **A real counterfactual simulator** — inserting synthetic orders directly
  into a message-driven order book reconstruction (already built for
  validation) so other simulated participants' reactions are modeled, not
  assumed away.
- **Passive/limit order strategies with queue-position modeling**, which
  would require modeling adverse selection honestly rather than sidestepping
  it as out of scope.
- **Multi-asset execution** — coordinating the schedule for a basket of
  correlated names instead of one stock at a time.

## Reference data

LOBSTER (lobsterdata.com) academic limit order book data, AAPL/INTC/GOOG,
2012-06-21. See [MODEL.md](MODEL.md) for the full derivation, calibration
methodology, and academic references.
