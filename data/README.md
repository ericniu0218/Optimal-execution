# Data directory

LOBSTER sample data for 2012-06-21 lives in `data/raw/` (gitignored).

Note: lobsterdata.com's original free-sample page no longer exists — the site
was rebuilt in ~2024 as a login/credit-gated platform. The identical original
sample files (same tickers, date, and filenames) are mirrored publicly at:

    https://huggingface.co/datasets/totalorganfailure/lobster-data

Download the `*_10` (level-10) folder per ticker, e.g.:

    LOBSTER_SampleFile_AAPL_2012-06-21_10/
      AAPL_2012-06-21_34200000_57600000_message_10.csv
      AAPL_2012-06-21_34200000_57600000_orderbook_10.csv

Ticker set used by this project: AAPL (primary backtest), INTC + GOOG
(liquidity contrast for impact calibration). Keep the original filenames —
the tooling parses ticker, date, and level count from them.

Verify a download with the smoke tool (parses the day, checks sentinel
normalization, crossed books, sweep statistics):

    build/oee_smoke data/raw/<msg>.csv data/raw/<book>.csv
