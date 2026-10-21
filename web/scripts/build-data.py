#!/usr/bin/env python3
"""Build the compact market-data pack the web demo replays.

Reads one LOBSTER day (message + orderbook CSVs) and writes a 1-second grid of
book snapshots, anchored on the first message timestamp:

    snapshot n  =  the book state after the LAST message with
                   ts <= open + n seconds

This is the exact lookup the C++ backtester performs (ExecutionSimulator::
row_at), so any strategy whose slice times land on whole seconds from `open`
sees byte-identical books in the browser and in C++. Per-second market volume
(type 4 + type 5 execution sizes, binned by timestamp) feeds the POV strategy.

Output
  public/data/<ticker>-<date>.v1.pack   gzip( int32 little-endian, column-major,
                                              each column delta-coded in time )
  public/data/meta.json                 layout description

Columns (each of length n):
  [ 0..L-1 ]      ask price, level 0..L-1   (ticks, 1e-4 dollars; 0 = empty)
  [ L..2L-1 ]     ask size
  [ 2L..3L-1 ]    bid price
  [ 3L..4L-1 ]    bid size
  [ 4L ]          market volume in [open+n, open+n+1)

Usage:  python3 -I web/scripts/build-data.py <message.csv> <orderbook.csv> <out_dir> TICKER YYYY-MM-DD
"""

import gzip
import json
import struct
import sys
from pathlib import Path

ASK_SENTINEL = 9999999999
BID_SENTINEL = -9999999999
NS = 1_000_000_000


def parse_ts_ns(text: str) -> int:
    """'34200.004241176' -> integer nanoseconds, exactly (no float round trip)."""
    whole, _, frac = text.partition(".")
    frac = (frac + "000000000")[:9]
    return int(whole) * NS + int(frac)


def main() -> None:
    msg_path, book_path, out_dir, ticker, date = sys.argv[1:6]
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)

    # ---- pass 1: message timestamps + per-second executed volume ------------
    ts: list[int] = []
    exec_events: list[tuple[int, int]] = []  # (ts_ns, size) for type 4 and 5
    with open(msg_path, "r", newline="") as f:
        for line in f:
            if not line.strip():
                continue
            parts = line.split(",")
            t = parse_ts_ns(parts[0])
            ts.append(t)
            if parts[1] in ("4", "5"):
                exec_events.append((t, int(parts[3])))

    rows = len(ts)
    open_ns = ts[0]
    n = (ts[-1] - open_ns) // NS + 1  # grid seconds 0 .. n-1 all have a row

    # ---- grid -> message row index (last row with ts <= open + k s) ----------
    sel = [0] * n
    i = 0
    for k in range(n):
        t_k = open_ns + k * NS
        while i + 1 < rows and ts[i + 1] <= t_k:
            i += 1
        sel[k] = i

    volume = [0] * n
    for t, size in exec_events:
        b = (t - open_ns) // NS
        if 0 <= b < n:
            volume[b] += size

    # ---- pass 2: parse only the orderbook rows the grid selects -------------
    needed = set(sel)
    book: dict[int, list[int]] = {}
    levels = 0
    with open(book_path, "r", newline="") as f:
        for idx, line in enumerate(f):
            if idx not in needed:
                continue
            fields = line.rstrip("\r\n").split(",")
            if not levels:
                levels = len(fields) // 4
            row = [int(x) for x in fields]
            ask_px, ask_sz, bid_px, bid_sz = [], [], [], []
            for l in range(levels):
                apx, asz, bpx, bsz = row[4 * l : 4 * l + 4]
                # Same normalization as the C++ reader: sentinel or empty -> (0, 0).
                if apx == ASK_SENTINEL or asz <= 0:
                    apx, asz = 0, 0
                if bpx == BID_SENTINEL or bsz <= 0:
                    bpx, bsz = 0, 0
                ask_px.append(apx)
                ask_sz.append(asz)
                bid_px.append(bpx)
                bid_sz.append(bsz)
            book[idx] = ask_px + ask_sz + bid_px + bid_sz

    ncols = 4 * levels + 1
    columns: list[list[int]] = [[0] * n for _ in range(ncols)]
    for k in range(n):
        vals = book[sel[k]]
        for c in range(4 * levels):
            columns[c][k] = vals[c]
        columns[4 * levels][k] = volume[k]

    # ---- encode: per-column delta, int32 LE, column-major, gzip -------------
    buf = bytearray()
    for col in columns:
        prev = 0
        deltas = []
        for v in col:
            deltas.append(v - prev)
            prev = v
        buf += struct.pack(f"<{n}i", *deltas)
    packed = gzip.compress(bytes(buf), compresslevel=9, mtime=0)

    name = f"{ticker.lower()}-{date}.v1.pack"
    (out / name).write_bytes(packed)

    meta = {
        "ticker": ticker,
        "date": date,
        "pack": name,
        "n": n,
        "levels": levels,
        "columns": ncols,
        "openNs": str(open_ns),  # string: exceeds 2^53 headroom concerns in JSON
        "openSecAfterMidnight": open_ns / NS,
        "messageRows": rows,
        "source": "LOBSTER sample data (lobsterdata.com), downsampled to a 1-second grid",
    }
    (out / "meta.json").write_text(json.dumps(meta, indent=2) + "\n")

    print(f"{ticker} {date}: {rows:,} message rows -> {n:,} one-second snapshots, {levels} levels")
    print(f"raw int32 {len(buf)/1e6:.2f} MB -> pack {len(packed)/1e6:.2f} MB  ({name})")


if __name__ == "__main__":
    main()
