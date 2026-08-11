#!/usr/bin/env python3
"""Render the multi-window distribution figures from oee_windows output:

  4. distributions.png     : per-strategy IS across all windows (strip +
                             mean/std), dispersion shrinking with lambda
  5. realized_frontier.png : realized (risk, impact-cost) per strategy vs
                             the analytic Almgren-Chriss frontier

Cost axis note: realized mean IS on a single trending day is confounded by
that day's drift (the model treats drift as zero-mean). The decomposition
lets us plot mean(IS - drift) — impact costs only — which is the quantity
the analytic E[IS] actually models. Raw IS is what the strip plot shows,
because dispersion is the point there.

Usage:
    plot_distributions.py --dir results/AAPL --ticker AAPL
"""

import argparse
import csv
from collections import defaultdict
from pathlib import Path
from statistics import mean, stdev

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

COLORS = {"AC": "#2a78d6", "TWAP": "#eb6834", "VWAP": "#1baf7a",
          "POV": "#eda100"}
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
BASELINE = "#c3c2b7"

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE,
    "savefig.facecolor": SURFACE, "text.color": INK,
    "axes.labelcolor": INK_2, "xtick.color": MUTED, "ytick.color": MUTED,
    "axes.edgecolor": BASELINE, "axes.grid": True, "grid.color": GRID,
    "grid.linewidth": 0.6, "axes.spines.top": False,
    "axes.spines.right": False, "font.family": "sans-serif",
    "font.size": 10, "axes.titlesize": 11, "legend.frameon": False,
    "lines.linewidth": 2.0,
})


def family(name):
    return "AC" if name.startswith("AC") else \
           "POV" if name.startswith("POV") else name


def load(out_dir):
    rows = list(csv.DictReader(open(out_dir / "windows.csv")))
    by = defaultdict(list)
    order = []
    for r in rows:
        if r["strategy"] not in order:
            order.append(r["strategy"])
        by[r["strategy"]].append(r)
    return order, by


def fig_distributions(out_dir, ticker, order, by):
    fig, ax = plt.subplots(figsize=(7.2, 4.6), dpi=150)
    for yi, name in enumerate(order):
        vals = [float(r["is_bps"]) for r in by[name]]
        color = COLORS[family(name)]
        # Strip of raw window outcomes (deterministic jitter for overlap).
        jitter = [((i * 7919) % 100 / 100.0 - 0.5) * 0.42
                  for i in range(len(vals))]
        ax.scatter(vals, [yi + j for j in jitter], s=14, color=color,
                   alpha=0.45, edgecolors="none")
        # Mean marker + +/-1 std whisker on top.
        m, s = mean(vals), stdev(vals)
        ax.plot([m - s, m + s], [yi, yi], color=color, lw=2.4,
                solid_capstyle="round")
        ax.plot([m], [yi], "o", ms=8, color=color, mec=SURFACE, mew=1.6,
                zorder=5)
        ax.annotate(f"{m:+.1f} ± {s:.1f}", (m, yi), xytext=(0, 11),
                    textcoords="offset points", ha="center", fontsize=8.5,
                    color=INK_2)

    ax.axvline(0, color=BASELINE, lw=1.0, zorder=0)
    ax.set_yticks(range(len(order)), order)
    ax.tick_params(axis="y", colors=INK)
    ax.invert_yaxis()
    n = len(next(iter(by.values())))
    ax.set_xlabel("implementation shortfall (bps) — one point per window")
    ax.set_title(f"{ticker}: IS distribution by strategy — dispersion "
                 f"shrinks as λ rises\n({n} rolling 30-min windows; "
                 "overlapping, hence serially correlated)", fontsize=10)
    fig.tight_layout()
    fig.savefig(out_dir / "figs" / "distributions.png")
    plt.close(fig)


def fig_realized_frontier(out_dir, ticker, order, by):
    frontier = list(csv.DictReader(open(out_dir / "frontier_windows.csv")))

    fig, ax = plt.subplots(figsize=(7.2, 4.6), dpi=150)
    fx = [float(r["std_bps"]) for r in frontier]
    fy = [float(r["expected_cost_bps"]) for r in frontier]
    ax.plot(fx, fy, color=COLORS["AC"], alpha=0.4, label="AC analytic frontier")

    ac_pts = []
    for name in order:
        vals_is = [float(r["is_bps"]) for r in by[name]]
        vals_net = [float(r["is_bps"]) - float(r["drift_bps"])
                    for r in by[name]]
        x, y = stdev(vals_is), mean(vals_net)
        color = COLORS[family(name)]
        if name.startswith("AC"):
            ac_pts.append((x, y, name))
        else:
            ax.plot([x], [y], "o", ms=8, color=color, mec=SURFACE, mew=1.5)
            # Stagger benchmark labels: VWAP goes up-left, TWAP up-right,
            # so neither hits the other or the AC lambda labels below.
            if name == "VWAP":
                off, ha = (-8, 6), "right"
            else:
                off, ha = (8, 6), "left"
            ax.annotate(name, (x, y), xytext=off,
                        textcoords="offset points", color=color, fontsize=9,
                        fontweight="bold", ha=ha)

    # AC lambda path: connected, lambda-labeled.
    ac_pts.sort(reverse=True)  # high risk (low lambda) -> low risk
    ax.plot([p[0] for p in ac_pts], [p[1] for p in ac_pts], "o-",
            color=COLORS["AC"], ms=8, mec=SURFACE, mew=1.5,
            label="AC realized (per λ)")
    for x, y, name in ac_pts:
        lam = name[3:-1]  # strip "AC(" and ")"
        ax.annotate(f"λ={lam}", (x, y), xytext=(6, -13),
                    textcoords="offset points", color=INK_2, fontsize=8.5)

    ax.set_xlabel("realized risk: std of IS across windows (bps)")
    ax.set_ylabel("realized impact cost:\nmean of IS − drift (bps)")
    ax.set_title(f"{ticker}: realized risk/cost per strategy vs the "
                 "analytic frontier", fontsize=10.5)
    ax.legend(loc="upper left")
    fig.tight_layout()
    fig.savefig(out_dir / "figs" / "realized_frontier.png")
    plt.close(fig)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dir", required=True)
    p.add_argument("--ticker", required=True)
    args = p.parse_args()
    out_dir = Path(args.dir)
    (out_dir / "figs").mkdir(exist_ok=True)

    order, by = load(out_dir)
    fig_distributions(out_dir, args.ticker, order, by)
    fig_realized_frontier(out_dir, args.ticker, order, by)
    print(f"wrote {out_dir}/figs/{{distributions,realized_frontier}}.png")


if __name__ == "__main__":
    main()
