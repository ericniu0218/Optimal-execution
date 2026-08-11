#!/usr/bin/env python3
"""Render the three Pass-1 figures from run_sweep.py output:

  1. trajectories.png : shares remaining vs time, all four strategies
  2. frontier.png     : analytic E[IS] vs sqrt(Var[IS]) efficient frontier
  3. is_vs_lambda.png : realized single-day AC IS vs lambda, against the
                        analytic expectation and the TWAP baseline

Usage:
    plot_results.py --dir results/AAPL --ticker AAPL [--qty 20000]
"""

import argparse
import csv
import json
import math
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import LogLocator

# Fixed entity -> hue mapping, constant across every figure (color follows
# the strategy, never its rank in a particular chart).
COLORS = {
    "AC": "#2a78d6",      # blue
    "TWAP": "#eb6834",    # orange
    "VWAP": "#1baf7a",    # aqua
    "POV": "#eda100",     # yellow
}
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
BASELINE = "#c3c2b7"

plt.rcParams.update({
    "figure.facecolor": SURFACE,
    "axes.facecolor": SURFACE,
    "savefig.facecolor": SURFACE,
    "text.color": INK,
    "axes.labelcolor": INK_2,
    "xtick.color": MUTED,
    "ytick.color": MUTED,
    "axes.edgecolor": BASELINE,
    "axes.grid": True,
    "grid.color": GRID,
    "grid.linewidth": 0.6,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "font.family": "sans-serif",
    "font.size": 10,
    "axes.titlesize": 11,
    "legend.frameon": False,
    "lines.linewidth": 2.0,
})


def read_rows(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def strategy_key(name):
    return "POV" if name.startswith("POV") else name


def fig_trajectories(out_dir, ticker, qty):
    slices = read_rows(out_dir / "slices.csv")
    by_strat = {}
    for r in slices:
        by_strat.setdefault(r["strategy"], []).append(r)

    fig, ax = plt.subplots(figsize=(7.2, 4.4), dpi=150)
    t0 = min(float(r["ts_sec"]) for r in slices)
    # Direct labels sit mid-curve at each line's most-separated region (the
    # lines all converge to 0% at the end, so end-of-line labels collide).
    label_at_min = {"AC": 16.0, "TWAP": 33.0, "VWAP": 46.0, "POV": 8.0}
    label_off = {"AC": (-6, -12), "TWAP": (4, 8), "VWAP": (6, 8),
                 "POV": (2, 10)}
    for name, rows in by_strat.items():
        key = strategy_key(name)
        ts = [0.0]
        rem = [100.0]
        filled = 0
        for r in sorted(rows, key=lambda r: int(r["slice_idx"])):
            filled += int(r["filled"])
            ts.append((float(r["ts_sec"]) - t0) / 60.0)
            rem.append(100.0 * (1.0 - filled / qty))
        ax.plot(ts, rem, color=COLORS[key], label=name,
                solid_capstyle="round")
        t_pick = label_at_min[key]
        i_pick = min(range(len(ts)), key=lambda i: abs(ts[i] - t_pick))
        ax.annotate(name, (ts[i_pick], rem[i_pick]),
                    xytext=label_off[key], textcoords="offset points",
                    color=COLORS[key], fontsize=9, fontweight="bold",
                    ha="left" if label_off[key][0] >= 0 else "right")

    ax.set_xlim(0, None)
    ax.set_ylim(0, 102)
    ax.set_xlabel("minutes since arrival")
    ax.set_ylabel("shares remaining (%)")
    ax.set_title(f"{ticker}: realized execution trajectories "
                 f"({qty:,} shares, fitted impact params)")
    ax.legend(loc="upper right")
    fig.tight_layout()
    fig.savefig(out_dir / "figs" / "trajectories.png")
    plt.close(fig)


def fig_frontier(out_dir, ticker):
    rows = read_rows(out_dir / "frontier.csv")
    x = [float(r["std_bps"]) for r in rows]
    y = [float(r["expected_cost_bps"]) for r in rows]
    lam = [float(r["lambda"]) for r in rows]

    fig, ax = plt.subplots(figsize=(7.2, 4.4), dpi=150)
    ax.plot(x, y, color=COLORS["AC"], solid_capstyle="round")

    # Markers at four lambdas; numeric labels only on the interior two —
    # the end regions carry prose captions instead (labels there collide).
    marks = [0, len(rows) // 3, 2 * len(rows) // 3, len(rows) - 1]
    for i in marks:
        ax.plot([x[i]], [y[i]], "o", ms=5, color=COLORS["AC"],
                mec=SURFACE, mew=1.5)
    for i in marks[1:-1]:
        ax.annotate(f"λ={lam[i]:.0e}", (x[i], y[i]), xytext=(8, 5),
                    textcoords="offset points", color=INK_2, fontsize=8.5)
    ax.annotate("slow (≈TWAP, λ→0):\nlow impact, high timing risk",
                (x[0], y[0]), xytext=(-14, 30), textcoords="offset points",
                color=MUTED, fontsize=8.5, ha="right")
    ax.annotate("fast (λ large):\nhigh impact, low timing risk",
                (x[-1], y[-1]), xytext=(16, -18), textcoords="offset points",
                color=MUTED, fontsize=8.5)

    ax.set_xlabel("timing risk sqrt(Var[IS])  (bps of arrival notional)")
    ax.set_ylabel("expected cost E[IS]  (bps)")
    ax.set_title(f"{ticker}: Almgren-Chriss efficient frontier "
                 "(analytic, fitted params)")
    fig.tight_layout()
    fig.savefig(out_dir / "figs" / "frontier.png")
    plt.close(fig)


def fig_is_vs_lambda(out_dir, ticker):
    sweep = read_rows(out_dir / "sweep.csv")
    frontier = read_rows(out_dir / "frontier.csv")
    summary = {strategy_key(r["strategy"]): r
               for r in read_rows(out_dir / "summary.csv")}

    fig, ax = plt.subplots(figsize=(7.2, 4.4), dpi=150)
    ax.set_xscale("log")

    fx = [float(r["lambda"]) for r in frontier]
    fy = [float(r["expected_cost_bps"]) for r in frontier]
    ax.plot(fx, fy, color=COLORS["AC"], alpha=0.45,
            label="AC analytic E[IS]")

    sx = [float(r["lambda"]) for r in sweep]
    sy = [float(r["is_bps"]) for r in sweep]
    ax.plot(sx, sy, "o-", color=COLORS["AC"], ms=6, mec=SURFACE, mew=1.2,
            label="AC realized (this day)")

    twap = float(summary["TWAP"]["is_bps"])
    ax.axhline(twap, color=COLORS["TWAP"], ls=(0, (4, 3)), lw=1.6,
               label=f"TWAP realized ({twap:+.1f} bps)")

    ax.set_xlabel("risk aversion λ")
    ax.set_ylabel("implementation shortfall (bps)")
    ax.set_title(f"{ticker}: cost vs risk aversion — one day is one draw "
                 "from the cost distribution")
    ax.xaxis.set_major_locator(LogLocator(base=10, numticks=12))
    ax.legend(loc="upper left")
    fig.tight_layout()
    fig.savefig(out_dir / "figs" / "is_vs_lambda.png")
    plt.close(fig)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dir", required=True)
    p.add_argument("--ticker", required=True)
    p.add_argument("--qty", type=int, default=None)
    args = p.parse_args()

    out_dir = Path(args.dir)
    (out_dir / "figs").mkdir(exist_ok=True)
    qty = args.qty or int(read_rows(out_dir / "summary.csv")[0]["qty"])

    fig_trajectories(out_dir, args.ticker, qty)
    fig_frontier(out_dir, args.ticker)
    fig_is_vs_lambda(out_dir, args.ticker)
    print(f"wrote {out_dir}/figs/{{trajectories,frontier,is_vs_lambda}}.png")


if __name__ == "__main__":
    main()
