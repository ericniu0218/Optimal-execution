#!/usr/bin/env python3
"""Orchestrate the analysis runs for one ticker:

  1. headline backtest at --headline-lambda  -> <out>/summary.csv, slices.csv
  2. realized IS at each lambda in the sweep -> <out>/sweep.csv
  3. analytic efficient frontier             -> <out>/frontier.csv

Reads fitted parameters from the calibration JSON so figures and backtests
can never drift out of sync with the calibration.

Usage:
    run_sweep.py --calib results/dump/AAPL/calib.json --ticker AAPL
                 --msg data/raw/AAPL_..._message_10.csv
                 --book data/raw/AAPL_..._orderbook_10.csv
                 --qty 20000 [--out-dir results/AAPL]
"""

import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path

LAMBDAS = [1e-11, 3e-11, 1e-10, 3e-10, 1e-9, 3e-9, 1e-8, 1e-7, 1e-6]


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"command failed: {' '.join(map(str, cmd))}\n{r.stderr}")
    return r.stdout


def read_summary(path):
    with open(path, newline="") as f:
        return {row["strategy"]: row for row in csv.DictReader(f)}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--calib", required=True)
    p.add_argument("--ticker", required=True)
    p.add_argument("--msg", required=True)
    p.add_argument("--book", required=True)
    p.add_argument("--qty", type=int, required=True)
    p.add_argument("--start-min", type=float, default=30.0)
    p.add_argument("--duration-min", type=float, default=60.0)
    p.add_argument("--slices", type=int, default=60)
    p.add_argument("--headline-lambda", type=float, default=3e-10)
    p.add_argument("--build-dir", default="build")
    p.add_argument("--out-dir", default=None)
    args = p.parse_args()

    calib = json.load(open(args.calib))
    gamma = calib["gamma_visible"]["gamma_ticks_per_share"]
    eta = calib["eta_ticks_per_share_per_min"]
    sigma = calib["sigma_ticks_per_sqrt_min"]
    epsilon = calib["median_half_spread_ticks"]

    out = Path(args.out_dir or f"results/{args.ticker}")
    out.mkdir(parents=True, exist_ok=True)
    bt = Path(args.build_dir) / "oee_backtest"
    fr = Path(args.build_dir) / "oee_frontier"

    def backtest(lam, out_dir):
        run([bt, "--msg", args.msg, "--book", args.book,
             "--qty", str(args.qty), "--start-min", str(args.start_min),
             "--duration-min", str(args.duration_min),
             "--slices", str(args.slices),
             "--gamma", str(gamma), "--eta", str(eta), "--sigma", str(sigma),
             "--epsilon", str(epsilon), "--lambda", str(lam),
             "--out-dir", str(out_dir)])
        return read_summary(Path(out_dir) / "summary.csv")

    # 1. Headline run (kept: summary.csv + slices.csv for trajectory figure).
    headline = backtest(args.headline_lambda, out)
    arrival_mid = float(next(iter(headline.values()))["arrival_mid_ticks"])
    print(f"headline lambda={args.headline_lambda}: " +
          ", ".join(f"{k} {float(v['is_bps']):+.2f}bps"
                    for k, v in headline.items()))

    # 2. Lambda sweep: realized AC IS per lambda (benchmarks are
    # lambda-independent and come from the headline run).
    tmp = out / "_sweep_tmp"
    with open(out / "sweep.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["lambda", "is_bps", "drift_bps", "permanent_bps",
                    "execution_bps", "opportunity_bps"])
        for lam in LAMBDAS:
            s = backtest(lam, tmp)
            if "AC" not in s:
                print(f"  lambda={lam:g}: AC ill-posed, skipped")
                continue
            r = s["AC"]
            w.writerow([lam, r["is_bps"], r["drift_bps"], r["permanent_bps"],
                        r["execution_bps"], r["opportunity_bps"]])
            print(f"  lambda={lam:g}: AC {float(r['is_bps']):+.2f} bps")

    # 3. Analytic frontier over a slightly wider lambda range.
    run([fr, "--qty", str(args.qty), "--duration-min", str(args.duration_min),
         "--slices", str(args.slices), "--gamma", str(gamma),
         "--eta", str(eta), "--sigma", str(sigma), "--epsilon", str(epsilon),
         "--arrival-mid", str(arrival_mid),
         "--lambda-min", str(LAMBDAS[0]), "--lambda-max", str(LAMBDAS[-1]),
         "--points", "40", "--out", str(out / "frontier.csv")])

    print(f"wrote {out}/summary.csv, slices.csv, sweep.csv, frontier.csv")


if __name__ == "__main__":
    main()
