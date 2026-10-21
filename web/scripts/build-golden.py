#!/usr/bin/env python3
"""Freeze the C++ engine's AAPL results into a golden fixture for the web tests.

The browser demo re-implements the backtester in TypeScript. This fixture is
what the port is held to: per-slice integers (child size, filled size, ladder
shift, pre-trade mid) must match the C++ output exactly, and the 71-window
distribution must match to CSV print precision.

Inputs (produced by the C++ tools; results/ is gitignored):
  results/AAPL/summary.csv   single window, qty 20000, start 30 min, 60 min, 60 slices, lambda 3e-10
  results/AAPL/slices.csv    its per-slice records
  results/AAPL/windows.csv   71 rolling windows, qty 10000, 30 min, 30 slices, 7 strategies

Usage: python3 -I web/scripts/build-golden.py results/AAPL web/src/engine/golden.json
"""

import csv
import json
import sys
from collections import defaultdict
from pathlib import Path


def read(path: Path) -> list[dict]:
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def main() -> None:
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])

    single = {}
    for r in read(src / "summary.csv"):
        single[r["strategy"]] = {
            "executed": int(r["executed"]),
            "remainder": int(r["remainder"]),
            "isBps": float(r["is_bps"]),
            "driftBps": float(r["drift_bps"]),
            "permanentBps": float(r["permanent_bps"]),
            "executionBps": float(r["execution_bps"]),
            "opportunityBps": float(r["opportunity_bps"]),
            "participation": float(r["participation"]),
        }

    slices = defaultdict(lambda: {"child": [], "filled": [], "shift": [], "mid2xBefore": []})
    for r in read(src / "slices.csv"):
        s = slices[r["strategy"]]
        s["child"].append(int(r["child"]))
        s["filled"].append(int(r["filled"]))
        s["shift"].append(int(r["shift_ticks"]))
        s["mid2xBefore"].append(int(float(r["mid2x_before"])))
    for name, s in slices.items():
        single[name]["slices"] = s

    windows = defaultdict(list)
    for r in read(src / "windows.csv"):
        windows[r["strategy"]].append(
            {
                "startMin": float(r["start_min"]),
                "isBps": float(r["is_bps"]),
                "driftBps": float(r["drift_bps"]),
                "permanentBps": float(r["permanent_bps"]),
                "executionBps": float(r["execution_bps"]),
                "opportunityBps": float(r["opportunity_bps"]),
            }
        )

    golden = {
        "note": "Frozen output of the C++ engine (oee_backtest / oee_windows) on AAPL 2012-06-21.",
        "params": {"gamma": 0.421774, "eta": 1.13567, "sigma": 2694.48, "epsilon": 750},
        "single": {
            "config": {"qty": 20000, "startMin": 30, "durationMin": 60, "slices": 60, "lambda": 3e-10, "pov": 0.05, "smile": 2.0},
            "strategies": single,
        },
        "windows": {
            "config": {"qty": 10000, "durationMin": 30, "slices": 30, "pov": 0.05, "smile": 2.0},
            "strategies": windows,
        },
    }
    dst.write_text(json.dumps(golden, separators=(",", ":")) + "\n")
    print(f"wrote {dst} ({dst.stat().st_size/1e3:.0f} KB): "
          f"{len(single)} single-window strategies, {len(windows)} window series x {len(next(iter(windows.values())))} windows")


if __name__ == "__main__":
    main()
