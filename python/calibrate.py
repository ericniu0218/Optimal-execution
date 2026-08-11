#!/usr/bin/env python3
"""Fit Almgren-Chriss impact parameters from oee_dump output.

Inputs (produced by `oee_dump`):
    events.csv : ts_sec, aggressor(+1/-1), qty, vwap_ticks, mid_before_ticks, hidden
    mids.csv   : ts_sec, mid_ticks, spread_ticks

Estimators (deliberately the simplest defensible version of each — Pass 1):

  sigma  Realized volatility of mid changes on RESAMPLE_SEC buckets,
         reported in ticks/sqrt(minute). Sampled sparsely rather than per
         event to limit microstructure-noise inflation (bid-ask bounce);
         still an upper bound at high frequency, noted in output.

  gamma  Permanent impact: OLS of the mid change over BUCKET_MIN buckets on
         net signed trade volume in the bucket (buyer-initiated positive),
         in ticks per share. Specification follows the order-flow-imbalance
         literature (Cont, Kukanov & Stoikov 2014). Reported with and
         without hidden executions.

  k      Temporary impact, MECHANICAL estimator: OLS of the depth-walk cost
         curve (depth.csv: average cost/share of consuming q shares of the
         visible book, from oee_dump) on q. This is planner-consistent by
         construction: the simulator charges exactly the book-walk cost, so
         this k calibrates the planner to the simulator's true mechanics.
         Sizes with fill_prob < FILL_PROB_MIN are excluded (partial-fill
         regime; different physics).

         The naive alternative — regressing per-event slippage on event
         size — is also computed and reported as a DIAGNOSTIC. It comes out
         with a small NEGATIVE slope and R2 ~ 0 on all tickers: aggressive
         traders condition size on available liquidity (big sweeps happen
         when the touch is deep), so selection bias swamps the causal
         impact slope. Kept as a demonstrated negative result.

  eta    The backtester executes each child as ONE sweep, so simulated
         temporary cost per share is eps + k*n. The AC planner's term is
         eta*(n/tau); these coincide when  eta = k * tau  (tau = slice
         length in minutes). We emit eta for the default tau and record k
         so other slicings can rescale.

Not calibrated: lambda. Risk aversion is a preference, not a market
parameter — sweep it for the efficient frontier instead.

Usage:
    calibrate.py --dump-dir results/dump/AAPL [--slice-min 1.0]
                 [--out calib.json] [--ticker AAPL]
"""

import argparse
import csv
import json
import math
import sys
from pathlib import Path

RESAMPLE_SEC = 10.0   # sigma sampling interval
BUCKET_MIN = 5.0      # gamma regression bucket length
TRIM_MIN = 5.0        # drop the first/last minutes (auction noise)
FILL_PROB_MIN = 0.95  # depth-curve sizes below this fill rate are excluded


def ols(xs, ys):
    """Simple OLS y = a + b*x. Returns (a, b, r2, n)."""
    n = len(xs)
    if n < 3:
        return float("nan"), float("nan"), float("nan"), n
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    syy = sum((y - my) ** 2 for y in ys)
    if sxx == 0 or syy == 0:
        return float("nan"), float("nan"), float("nan"), n
    b = sxy / sxx
    a = my - b * mx
    r2 = (sxy * sxy) / (sxx * syy)
    return a, b, r2, n


def read_csv(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        sys.exit(f"calibrate: {path} is empty")
    return rows


def fit_sigma(mids):
    """Realized vol of mid changes, ticks/sqrt(min)."""
    # mids.csv is on a fixed grid; resample by stride.
    t0 = mids[0]["ts_sec"]
    grid = mids[1]["ts_sec"] - t0
    stride = max(1, round(RESAMPLE_SEC / grid))
    sampled = mids[::stride]
    diffs = [b["mid_ticks"] - a["mid_ticks"] for a, b in zip(sampled, sampled[1:])]
    n = len(diffs)
    mean = sum(diffs) / n
    var = sum((d - mean) ** 2 for d in diffs) / (n - 1)
    per_bucket = math.sqrt(var)
    return per_bucket * math.sqrt(60.0 / (grid * stride)), n


def fit_gamma(events, mids, include_hidden):
    """OLS mid change on net signed volume over BUCKET_MIN buckets."""
    mid_at = {round(m["ts_sec"], 3): m["mid_ticks"] for m in mids}
    t_start = mids[0]["ts_sec"]
    t_end = mids[-1]["ts_sec"]
    bucket = BUCKET_MIN * 60.0

    xs, ys = [], []
    t = t_start
    ev_i = 0
    events_sorted = events  # already in tape order
    while t + bucket <= t_end:
        # Net signed flow in [t, t+bucket).
        q = 0
        while ev_i < len(events_sorted) and events_sorted[ev_i]["ts_sec"] < t + bucket:
            ev = events_sorted[ev_i]
            if ev["ts_sec"] >= t and (include_hidden or not ev["hidden"]):
                q += ev["aggressor"] * ev["qty"]
            ev_i += 1
        m0 = mid_at.get(round(t, 3))
        m1 = mid_at.get(round(t + bucket, 3))
        if m0 is not None and m1 is not None:
            xs.append(q)
            ys.append(m1 - m0)
        t += bucket
    a, b, r2, n = ols(xs, ys)
    return {"gamma_ticks_per_share": b, "intercept_drift_ticks": a,
            "r2": r2, "n_buckets": n}


def fit_temporary_event_diagnostic(events, include_hidden):
    """OLS per-event slippage on event size. DIAGNOSTIC ONLY: selection
    bias (aggressors size to the book) makes this slope unusable for the
    planner — it typically comes out slightly negative with R2 ~ 0."""
    xs, ys = [], []
    for ev in events:
        if ev["hidden"] and not include_hidden:
            continue
        slippage = ev["aggressor"] * (ev["vwap_ticks"] - ev["mid_before_ticks"])
        xs.append(ev["qty"])
        ys.append(slippage)
    a, b, r2, n = ols(xs, ys)
    return {"epsilon_ticks": a, "k_ticks_per_share": b, "r2": r2, "n_events": n}


def fit_temporary_mechanical(depth_rows):
    """OLS of the depth-walk cost curve on consumption size — the
    planner-consistent temporary-impact estimator."""
    used = [r for r in depth_rows if r["fill_prob"] >= FILL_PROB_MIN
            and r["mean_cost"] >= 0]
    xs = [r["qty"] for r in used]
    ys = [r["mean_cost"] for r in used]
    a, b, r2, n = ols(xs, ys)
    return {"epsilon_ticks": a, "k_ticks_per_share": b, "r2": r2,
            "n_sizes_used": n,
            "max_qty_used": max(xs) if xs else 0,
            "curve": [{"qty": r["qty"], "cost": r["mean_cost"],
                       "fill_prob": r["fill_prob"]} for r in depth_rows]}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dump-dir", required=True)
    p.add_argument("--slice-min", type=float, default=1.0,
                   help="slice length tau the backtest will use (minutes)")
    p.add_argument("--out", default=None)
    p.add_argument("--ticker", default="?")
    args = p.parse_args()

    dump = Path(args.dump_dir)
    events = [
        {"ts_sec": float(r["ts_sec"]), "aggressor": int(r["aggressor"]),
         "qty": int(r["qty"]), "vwap_ticks": float(r["vwap_ticks"]),
         "mid_before_ticks": float(r["mid_before_ticks"]),
         "hidden": r["hidden"] == "1"}
        for r in read_csv(dump / "events.csv")
    ]
    mids = [
        {"ts_sec": float(r["ts_sec"]), "mid_ticks": float(r["mid_ticks"]),
         "spread_ticks": float(r["spread_ticks"])}
        for r in read_csv(dump / "mids.csv")
    ]
    depth_rows = [
        {"qty": int(r["qty"]),
         "mean_cost": float(r["mean_cost_ticks_per_share"]),
         "fill_prob": float(r["fill_prob"])}
        for r in read_csv(dump / "depth.csv")
    ]

    # Trim auction-adjacent noise at both ends of the session.
    t_lo = mids[0]["ts_sec"] + TRIM_MIN * 60.0
    t_hi = mids[-1]["ts_sec"] - TRIM_MIN * 60.0
    events = [e for e in events if t_lo <= e["ts_sec"] <= t_hi]
    mids = [m for m in mids if t_lo <= m["ts_sec"] <= t_hi]

    sigma, n_sigma = fit_sigma(mids)
    spreads = sorted(m["spread_ticks"] for m in mids)
    median_half_spread = spreads[len(spreads) // 2] / 2.0

    gamma_vis = fit_gamma(events, mids, include_hidden=False)
    gamma_all = fit_gamma(events, mids, include_hidden=True)
    mech = fit_temporary_mechanical(depth_rows)
    diag_vis = fit_temporary_event_diagnostic(events, include_hidden=False)
    diag_all = fit_temporary_event_diagnostic(events, include_hidden=True)

    eta = mech["k_ticks_per_share"] * args.slice_min

    calib = {
        "ticker": args.ticker,
        "slice_min_assumed": args.slice_min,
        "sigma_ticks_per_sqrt_min": sigma,
        "sigma_n_buckets": n_sigma,
        "median_half_spread_ticks": median_half_spread,
        "gamma_visible": gamma_vis,
        "gamma_with_hidden": gamma_all,
        "temporary_mechanical": mech,
        "temporary_event_diagnostic_visible": diag_vis,
        "temporary_event_diagnostic_with_hidden": diag_all,
        "eta_ticks_per_share_per_min": eta,
        "notes": [
            "sigma from {}s mid changes; microstructure noise biases high".format(RESAMPLE_SEC),
            "gamma: mid change on net signed flow, {}min buckets".format(BUCKET_MIN),
            "eta = k_mechanical * tau; rescale via k if slicing changes",
            "event-slippage regression kept as diagnostic only: selection "
            "bias (aggressors size to the book) drives its slope ~<= 0",
            "AC assumes LINEAR impact; the depth curve is convex in q over "
            "the fitted range, so linear k understates small-q and "
            "overstates mid-q cost — fitted range recorded in "
            "temporary_mechanical.max_qty_used",
        ],
    }

    out_path = args.out or (dump / "calib.json")
    with open(out_path, "w") as f:
        json.dump(calib, f, indent=2)

    print(f"== {args.ticker} calibration ==")
    print(f"sigma   : {sigma:10.2f} ticks/sqrt(min)   ({n_sigma} buckets)")
    print(f"gamma   : {gamma_vis['gamma_ticks_per_share']:10.6f} ticks/share      "
          f"(R2={gamma_vis['r2']:.3f}, n={gamma_vis['n_buckets']}; "
          f"with hidden: {gamma_all['gamma_ticks_per_share']:.6f})")
    print(f"k mech  : {mech['k_ticks_per_share']:10.6f} ticks/share      "
          f"(R2={mech['r2']:.3f}, fitted q<={mech['max_qty_used']}; "
          f"eps_mech={mech['epsilon_ticks']:.2f})")
    print(f"k event : {diag_vis['k_ticks_per_share']:10.6f} ticks/share      "
          f"(R2={diag_vis['r2']:.3f}, n={diag_vis['n_events']}) "
          f"[DIAGNOSTIC: selection-biased, not used]")
    print(f"epsilon : {mech['epsilon_ticks']:10.2f} ticks (depth-curve intercept) vs "
          f"{median_half_spread:.2f} (median quoted half-spread)")
    print(f"eta     : {eta:10.4f} ticks/(share/min) at tau={args.slice_min}min")
    print(f"wrote {out_path}")
    print("\nsuggested backtest flags:")
    print(f"  --gamma {gamma_vis['gamma_ticks_per_share']:.6g} "
          f"--eta {eta:.6g} --sigma {sigma:.6g} "
          f"--epsilon {median_half_spread:.6g}")


if __name__ == "__main__":
    main()
