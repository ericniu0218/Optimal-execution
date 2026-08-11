// Export calibration inputs from a LOBSTER day:
//
//   <out-dir>/events.csv : one row per aggregated trade event
//       ts_sec, aggressor(+1/-1), qty, vwap_ticks, mid_before_ticks, hidden
//   <out-dir>/mids.csv   : mid/spread sampled on a fixed time grid
//       ts_sec, mid_ticks, spread_ticks
//   <out-dir>/depth.csv  : mechanical cost-of-consumption curve
//       qty, mean_cost_ticks_per_share, fill_prob, n_snapshots
//       (average over sampled snapshots of the depth-walk cost of consuming
//        qty shares, both sides pooled, relative to the prevailing mid)
//
// The depth curve is the planner-consistent temporary-impact estimator: the
// simulator charges exactly the book-walk cost, so fitting eps + k*q to
// this curve calibrates the planner to the simulator's actual mechanics.
// The event-level slippage regression (done in Python from events.csv) is
// kept as a diagnostic; it is polluted by liquidity-timing selection bias
// (aggressors size to the book), which the write-up demonstrates.
//
// The Python layer does regression ONLY — sweep aggregation, hidden-order
// classification, mid construction, and depth walking stay in C++ so there
// is exactly one implementation of the trap-prone logic (the tested one).
//
// Usage: oee_dump --msg <message.csv> --book <orderbook.csv> --out-dir <dir>
//                 [--mid-grid-sec 1] [--depth-sample-sec 10]

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "oee/data/tape_cache.hpp"
#include "oee/data/trade_events.hpp"

int main(int argc, char** argv) {
  std::string msg_path, book_path, out_dir = "results/dump";
  double grid_sec = 1.0;
  double depth_sample_sec = 10.0;
  for (int i = 1; i < argc; ++i) {
    const std::string f = argv[i];
    auto need = [&]() -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", f.c_str()); std::exit(1); }
      return argv[++i];
    };
    if (f == "--msg") msg_path = need();
    else if (f == "--book") book_path = need();
    else if (f == "--out-dir") out_dir = need();
    else if (f == "--mid-grid-sec") grid_sec = std::atof(need());
    else if (f == "--depth-sample-sec") depth_sample_sec = std::atof(need());
    else { std::fprintf(stderr, "unknown flag: %s\n", f.c_str()); std::exit(1); }
  }
  if (msg_path.empty() || book_path.empty()) {
    std::fprintf(stderr, "required: --msg <message.csv> --book <orderbook.csv>\n");
    return 1;
  }

  try {
    const oee::LobsterDay day = oee::load_day_cached(msg_path, book_path);
    std::filesystem::create_directories(out_dir);

    // ---- events.csv -----------------------------------------------------
    const auto events = oee::build_trade_events(day.messages, day.book);
    std::ofstream ev_out(out_dir + "/events.csv");
    ev_out << "ts_sec,aggressor,qty,vwap_ticks,mid_before_ticks,hidden\n";
    std::size_t skipped = 0;
    for (const oee::TradeEvent& ev : events) {
      // Need the pre-event mid (book state before the first leg) and a
      // classified aggressor; skip the rare rest.
      if (ev.first_row == 0 || ev.aggressor == oee::Aggressor::kUnknown) {
        ++skipped;
        continue;
      }
      const std::size_t prev = ev.first_row - 1;
      if (!day.book.has_best_ask(prev) || !day.book.has_best_bid(prev)) {
        ++skipped;
        continue;
      }
      const double mid_before =
          static_cast<double>(day.book.mid2x(prev)) / 2.0;
      ev_out << static_cast<double>(ev.ts) / 1e9 << ','
             << static_cast<int>(ev.aggressor) << ',' << ev.total_qty << ','
             << ev.vwap_ticks() << ',' << mid_before << ','
             << (ev.hidden ? 1 : 0) << '\n';
    }

    // ---- mids.csv -------------------------------------------------------
    const auto& ts = day.messages.ts;
    const oee::Nanos step = static_cast<oee::Nanos>(grid_sec * 1e9);
    std::ofstream mid_out(out_dir + "/mids.csv");
    mid_out << "ts_sec,mid_ticks,spread_ticks\n";
    std::size_t grid_points = 0;
    for (oee::Nanos t = ts.front(); t <= ts.back(); t += step) {
      const auto it = std::upper_bound(ts.begin(), ts.end(), t);
      const std::size_t row = static_cast<std::size_t>(it - ts.begin()) - 1;
      if (!day.book.has_best_ask(row) || !day.book.has_best_bid(row)) continue;
      mid_out << static_cast<double>(t) / 1e9 << ','
              << static_cast<double>(day.book.mid2x(row)) / 2.0 << ','
              << day.book.best_ask(row) - day.book.best_bid(row) << '\n';
      ++grid_points;
    }

    // ---- depth.csv ------------------------------------------------------
    // For each sampled snapshot and each q in the grid, walk both sides of
    // the book and record cost per share vs the mid. Pooling buy/sell
    // symmetrizes; snapshots with insufficient visible depth count toward
    // fill_prob but not toward the cost mean (the simulator would partially
    // fill there, a different regime worth seeing separately).
    const std::vector<oee::Qty> q_grid = {50,   100,  250,  500,  1000,
                                          2000, 3500, 5000, 7500, 10000};
    const oee::Nanos depth_step = static_cast<oee::Nanos>(depth_sample_sec * 1e9);
    struct Acc { double cost_sum = 0; long n_full = 0; long n_total = 0; };
    std::vector<Acc> acc(q_grid.size());

    for (oee::Nanos t = ts.front(); t <= ts.back(); t += depth_step) {
      const auto it = std::upper_bound(ts.begin(), ts.end(), t);
      const std::size_t row = static_cast<std::size_t>(it - ts.begin()) - 1;
      if (!day.book.has_best_ask(row) || !day.book.has_best_bid(row)) continue;
      const double mid = static_cast<double>(day.book.mid2x(row)) / 2.0;

      for (std::size_t gi = 0; gi < q_grid.size(); ++gi) {
        const oee::Qty q = q_grid[gi];
        // Walk one side: returns (notional, filled).
        const auto walk = [&](bool sell) {
          std::int64_t notional = 0;
          oee::Qty rem = q;
          for (int l = 0; l < day.book.levels && rem > 0; ++l) {
            const oee::PriceTicks px =
                sell ? day.book.bid_px(row, l) : day.book.ask_px(row, l);
            const oee::Qty sz =
                sell ? day.book.bid_sz(row, l) : day.book.ask_sz(row, l);
            if (px == oee::kNullPrice || sz <= 0) break;
            const oee::Qty take = std::min(rem, sz);
            notional += static_cast<std::int64_t>(px) * take;
            rem -= take;
          }
          return std::pair<std::int64_t, oee::Qty>(notional, q - rem);
        };
        const auto [sell_notional, sell_filled] = walk(true);
        const auto [buy_notional, buy_filled] = walk(false);
        acc[gi].n_total += 2;
        if (sell_filled == q) {
          acc[gi].cost_sum += mid - static_cast<double>(sell_notional) / q;
          acc[gi].n_full += 1;
        }
        if (buy_filled == q) {
          acc[gi].cost_sum += static_cast<double>(buy_notional) / q - mid;
          acc[gi].n_full += 1;
        }
      }
    }

    std::ofstream depth_out(out_dir + "/depth.csv");
    depth_out << "qty,mean_cost_ticks_per_share,fill_prob,n_snapshots\n";
    for (std::size_t gi = 0; gi < q_grid.size(); ++gi) {
      const Acc& a = acc[gi];
      depth_out << q_grid[gi] << ','
                << (a.n_full > 0 ? a.cost_sum / static_cast<double>(a.n_full) : -1.0)
                << ','
                << (a.n_total > 0
                        ? static_cast<double>(a.n_full) / static_cast<double>(a.n_total)
                        : 0.0)
                << ',' << a.n_total / 2 << '\n';
    }

    // ---- decay.csv ------------------------------------------------------
    // Empirical impact decay: after a trade, how much of the immediate mid
    // move survives at horizon h? Averaging the AGGRESSOR-SIGNED response
    // over many events cancels unrelated drift and leaves the systematic
    // impact profile. The curve starts at the immediate response and
    // relaxes toward the permanent fraction; fitting
    //     R(h)/R(0) = f_perm + (1 - f_perm) * exp(-rho * h)
    // (done in Python) yields both the resilience rate rho and an
    // INDEPENDENT estimate of what share of impact is permanent — a direct
    // cross-check on the flow-regression gamma.
    // CRITICAL subtlety, found by running this on real data: the
    // unconditional response function RISES with horizon instead of
    // decaying. That is not resilience failing to exist — it is order-flow
    // autocorrelation. A trade is typically one slice of somebody's
    // metaorder, so more same-side flow follows and keeps pushing the mid.
    // The unconditional curve therefore measures "impact of this trade AND
    // its correlated successors" (the motivation for propagator models,
    // Bouchaud et al. 2004).
    //
    // To see resilience we condition on ISOLATED trades: for horizon h,
    // include an event only if NO other trade occurred in (t, t+h]. Then
    // the observed relaxation belongs to that trade alone. Both curves are
    // emitted — the contrast is the point.
    const std::vector<double> horizons = {0.0,  0.1,  0.25, 0.5, 1.0,  2.0,
                                          5.0, 10.0, 30.0, 60.0, 120.0};
    std::vector<double> resp_sum(horizons.size(), 0.0);
    std::vector<long> resp_n(horizons.size(), 0);
    std::vector<double> iso_sum(horizons.size(), 0.0);
    std::vector<long> iso_n(horizons.size(), 0);
    std::vector<double> depth_ratio_sum(horizons.size(), 0.0);
    std::vector<long> depth_ratio_n(horizons.size(), 0);

    for (std::size_t ei = 0; ei < events.size(); ++ei) {
      const oee::TradeEvent& ev = events[ei];
      if (ev.hidden || ev.aggressor == oee::Aggressor::kUnknown) continue;
      if (ev.first_row == 0) continue;
      const std::size_t prev = ev.first_row - 1;
      if (!day.book.has_best_ask(prev) || !day.book.has_best_bid(prev)) continue;
      const double mid_before = static_cast<double>(day.book.mid2x(prev)) / 2.0;
      const double sign = static_cast<double>(static_cast<int>(ev.aggressor));
      // Time to the next trade of ANY kind (hidden flow moves prices too).
      const double gap_sec =
          ei + 1 < events.size()
              ? static_cast<double>(events[ei + 1].ts - ev.ts) / 1e9
              : std::numeric_limits<double>::infinity();

      // Depth resilience: total visible depth on the side that was consumed
      // (a buyer-initiated trade eats asks), before the trade vs at t+h.
      const bool aggressed_asks = ev.aggressor == oee::Aggressor::kBuyer;
      const auto side_depth = [&](std::size_t row) {
        oee::Qty total = 0;
        for (int l = 0; l < day.book.levels; ++l) {
          total += aggressed_asks ? day.book.ask_sz(row, l)
                                  : day.book.bid_sz(row, l);
        }
        return total;
      };
      const oee::Qty depth_before = side_depth(prev);

      for (std::size_t hi = 0; hi < horizons.size(); ++hi) {
        const oee::Nanos t_h =
            ev.ts + static_cast<oee::Nanos>(horizons[hi] * 1e9);
        if (t_h > ts.back()) continue;
        const auto it = std::upper_bound(ts.begin(), ts.end(), t_h);
        const std::size_t row = static_cast<std::size_t>(it - ts.begin()) - 1;
        if (!day.book.has_best_ask(row) || !day.book.has_best_bid(row)) continue;
        const double mid_h = static_cast<double>(day.book.mid2x(row)) / 2.0;
        const double response = sign * (mid_h - mid_before);
        resp_sum[hi] += response;
        ++resp_n[hi];
        if (gap_sec > horizons[hi]) {
          iso_sum[hi] += response;
          ++iso_n[hi];
          if (depth_before > 0) {
            depth_ratio_sum[hi] += static_cast<double>(side_depth(row)) /
                                   static_cast<double>(depth_before);
            ++depth_ratio_n[hi];
          }
        }
      }
    }

    std::ofstream decay_out(out_dir + "/decay.csv");
    decay_out << "horizon_sec,mean_response_ticks,n_events,"
                 "mean_response_isolated_ticks,n_isolated,"
                 "mean_depth_ratio,n_depth\n";
    for (std::size_t hi = 0; hi < horizons.size(); ++hi) {
      decay_out << horizons[hi] << ','
                << (resp_n[hi] > 0
                        ? resp_sum[hi] / static_cast<double>(resp_n[hi])
                        : 0.0)
                << ',' << resp_n[hi] << ','
                << (iso_n[hi] > 0 ? iso_sum[hi] / static_cast<double>(iso_n[hi])
                                  : 0.0)
                << ',' << iso_n[hi] << ','
                << (depth_ratio_n[hi] > 0
                        ? depth_ratio_sum[hi] /
                              static_cast<double>(depth_ratio_n[hi])
                        : 0.0)
                << ',' << depth_ratio_n[hi] << '\n';
    }

    std::printf("wrote %s/events.csv (%zu events, %zu skipped), "
                "%s/mids.csv (%zu points @ %.3gs), "
                "%s/depth.csv (%zu sizes @ %.3gs sampling), "
                "%s/decay.csv (%zu horizons)\n",
                out_dir.c_str(), events.size() - skipped, skipped,
                out_dir.c_str(), grid_points, grid_sec, out_dir.c_str(),
                q_grid.size(), depth_sample_sec, out_dir.c_str(),
                horizons.size());
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAILED: %s\n", e.what());
    return 1;
  }
}
