# Model

Full derivation and calibration methodology, split out of the README so the
top-level document stays skimmable. This assumes familiarity with the
Almgren-Chriss framework; the README's "How it works" section is the
plain-English version.

## Setup and notation

Liquidate `X` shares over `[0, T]` in `N` intervals of length `τ = T/N`.
Holdings `x_0 = X ≥ x_1 ≥ ... ≥ x_N = 0`; the trade in interval `k` is
`n_k = x_{k-1} - x_k`.

Price dynamics with linear permanent impact `γ` and linear temporary impact
`η` (plus a fixed cost `ε`, typically the half-spread):

    S_k = S_{k-1} + σ√τ·ξ_k − γ·n_k                     (price after interval k)
    S̃_k = S_{k-1} − (ε·sgn(n_k) + (η/τ)·n_k)            (execution price in interval k)

Implementation shortfall `IS = X·S_0 − Σ n_k·S̃_k` has closed-form moments:

    E[IS] = (γ/2)X²  +  ε·Σ|n_k|  +  (η̃/τ)·Σn_k²
    V[IS] = σ²τ·Σ_{k=1}^{N-1} x_k²
    η̃ = η − γτ/2                       (discreteness correction)

`η̃`, not raw `η`, is what appears everywhere below — using raw `η` silently
mixes the discrete recursion with its continuous-time limit and breaks any
tight numerical comparison against an independent solve of the same
objective.

## The optimal trajectory

Minimizing `E[IS] + λ·V[IS]` over `{x_j}` gives the linear second-order
difference equation

    (x_{j-1} − 2x_j + x_{j+1}) / τ²  =  κ̃² x_j ,     κ̃² = λσ² / η̃

whose decay rate `κ` solves `2(cosh(κτ) − 1)/τ² = κ̃²`, i.e.

    κ = (2/τ)·asinh(κ̃τ/2)                              [exact discrete]

(`κ → κ̃` as `τ → 0`; the solver exposes both via a flag so the size of the
discreteness correction is a one-line experiment, not a code change.) With
boundary conditions `x_0 = X, x_N = 0`:

    x_j = X · sinh(κ(T − t_j)) / sinh(κT)

`λ = 0 ⟹ κ = 0 ⟹ x_j → X(1 − t_j/T)` — TWAP is the risk-neutral limit of
the same formula, not a separately-coded special case.

**Well-posedness.** The problem requires `η̃ > 0`, i.e. `η > γτ/2`: temporary
impact must dominate permanent impact at the chosen slice length. If the
calibrated `γ` is large relative to `η` (as happens for INTC — see
Calibration below), no slicing makes the discrete problem convex, and the
solver throws rather than returning a degenerate trajectory.

## Drift extension (adaptive execution)

Re-solving the drift-free problem on the remaining shares each period is
provably vacuous: under mean-variance the re-solve is time-inconsistent, and
under CARA utility with Gaussian prices the optimal policy is *deterministic*
(Schied & Schöneborn, 2009) — no realized price path should change the
schedule. Real adaptivity needs new information the static solve did not
have, so this adds a drift term `α` (expected price change per unit time,
signed so `α > 0` means "price expected to rise," a headwind for a seller):

The cost functional gains `− α·τ·Σ_{k=1}^{N-1} x_k` (holding `x_k` through
interval `k+1` earns `α·τ` per share), so the first-order condition keeps the
same linear recursion with a constant forcing term:

    (x_{j-1} − 2x_j + x_{j+1}) / τ²  =  κ̃² x_j − α / (2η̃)

whose particular solution is the constant `x̄ = α / (2λσ²)` — the inventory
the drift alone would justify holding — plus the homogeneous solution:

    x_j = x̄ + u·e^(−κt_j) − (x̄ + u·e^(−κT))·sinh(κt_j)/sinh(κT),   u = X − x̄

(`α = 0` collapses this to the plain solution exactly.) The risk-neutral
case (`λ = 0`) is separate and quadratic:
`x_j = X(1 − t/T) + [α/(4η̃)]·t(T − t)` — a parabola bowing the TWAP line,
vanishing at both endpoints by construction.

`x̄`'s denominator is `2λσ²`, which is small at realistic `λ` — an unshrunk
drift estimate can imply a target inventory far larger than the whole order.
The implementation caps `|x̄|` at a fraction of shares remaining; see the
README's adaptive-execution section for the measured consequence of skipping
that cap.

## Transient impact (Obizhaeva-Wang resilience)

Permanent impact (above) never decays. Real impact partially decays as the
book replenishes. The transient extension:

    shift(t) = γ·Q(t)  +  κ_r·Σᵢ qᵢ·e^(−ρ(t−tᵢ))

nests both models used elsewhere in this project: `ρ → ∞` collapses the sum
to zero instantly (pure permanent impact, what the headline backtests use);
`ρ → 0` never decays (merges into `γ`). Half-life is `ln(2)/ρ`. See the
README for the empirical measurement of whether `ρ` matters on this data.

## Numerics

- **Stable sinh ratio.** `sinh(a)/sinh(b)` for `0 ≤ a ≤ b` is evaluated as
  `e^(a−b)·expm1(−2a)/expm1(−2b)`. Naive evaluation overflows for `κT ≳ 710`
  and cancels catastrophically as `κT → 0`; this form is exact at both
  extremes since `expm1` has no cancellation near zero and every exponent
  stays `≤ 0`.
- **λ = 0.** Handled as an explicit branch (exact straight line / parabola),
  not as a limit of the general formula — `κ = 0` makes the general form's
  `0/0`.
- **Validation.** The closed form is checked against an independently-coded
  tridiagonal QP solve (Thomas algorithm) of the identical discrete
  objective, sharing no code path with the `sinh` derivation. Agreement is
  better than `1e-11` relative across `λ ∈ [10⁻⁸, 10⁻⁵]`, `N ∈ [4, 390]`, and
  (with the drift extension) `α ∈ [−0.05, 0.05]`.

## Calibration methodology

Three parameters are fit from the LOBSTER message/orderbook files for each
ticker/day (`oee_dump` exports the regression inputs in tested C++; fitting
itself is plain-stdlib Python in `python/calibrate.py`):

- **σ** — realized volatility of mid-price changes sampled every 10 seconds
  (sparse sampling to limit bid-ask-bounce noise), annualized to
  ticks/√minute.
- **γ** — OLS of the mid-price change over 5-minute buckets on net signed
  trade volume in that bucket (buyer-initiated positive). Direction is
  recovered via the Lee-Ready quote rule / tick test for hidden executions,
  whose direction the raw feed does not carry.
- **η** — **not** fit from a naive regression of per-trade slippage on trade
  size (that estimator has R² ≈ 0 across all three tickers — see the
  README's engineering section for why). Instead: walk the historical book
  at each depth in a grid and record the realized cost of consuming that
  much visible liquidity, averaged over sampled snapshots; fit `ε + k·q` to
  that curve. `η = k · τ` for the slice length in use. This estimator is
  planner-consistent by construction, since the execution simulator charges
  exactly this book-walk cost.

`λ` is not calibrated — it is a stated risk preference, swept for the
efficient frontier rather than fit to data.

## References

- Almgren, R. and N. Chriss (2000). *Optimal Execution of Portfolio
  Transactions.* Journal of Risk 3, 5-39.
- Almgren, R., Thum, C., Hauptmann, E., and H. Li (2005). *Direct Estimation
  of Equity Market Impact.* Risk 18, 58-62.
- Obizhaeva, A. and J. Wang (2013). *Optimal Trading Strategy and Supply/
  Demand Dynamics.* Journal of Financial Markets 16(1), 1-32.
- Schied, A. and T. Schöneborn (2009). *Risk Aversion and the Dynamics of
  Optimal Liquidation Strategies in Illiquid Markets.* Finance and
  Stochastics 13(2), 181-204.
- Bouchaud, J.-P., Gefen, Y., Potters, M., and M. Wyart (2004). *Fluctuations
  and Response in Financial Markets: The Subtle Nature of "Random" Price
  Changes.* Quantitative Finance 4(2), 176-190.
- Lee, C. and M. Ready (1991). *Inferring Trade Direction from Intraday
  Data.* Journal of Finance 46(2), 733-746.
- Cont, R., Kukanov, A., and S. Stoikov (2014). *The Price Impact of Order
  Book Events.* Journal of Financial Econometrics 12(1), 47-88.
