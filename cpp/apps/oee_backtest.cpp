// Run TWAP / VWAP / POV / Almgren-Chriss over the same parent order on one
// replayed LOBSTER day and compare implementation shortfall.
//
// Impact parameters default to placeholders (clearly printed) until the
// Python calibration layer produces fitted values; pass --gamma/--eta/
// --sigma to override. Units: ticks (1e-4 dollars) and minutes.
//
// Usage:
//   oee_backtest --msg <message.csv> --book <orderbook.csv> [options]
//
// Options (defaults in brackets):
//   --side sell|buy      [sell]     --qty N          [100000]
//   --start-min M        [30]       --duration-min M [60]
//   --slices N           [60]       --lambda L       [1e-9]
//   --gamma G  ticks/share [0.01]   --eta E ticks/(share/min) [0.5]
//   --sigma S  ticks/sqrt(min) [4000]
//   --epsilon F ticks    [-1 = half-spread at arrival]
//   --pov P              [0.05]     --smile S        [2.0]
//   --kappa K  transient impact ticks/share [0]
//   --rho R    resilience 1/sec [0 = transient never decays]
//   --out-dir D          [results]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "oee/backtest/backtester.hpp"
#include "oee/data/tape_cache.hpp"
#include "oee/strategy/pov.hpp"
#include "oee/strategy/scheduled.hpp"

namespace {

struct Args {
  std::string msg_path, book_path;
  std::string side = "sell";
  long long qty = 100000;
  double start_min = 30.0, duration_min = 60.0;
  int slices = 60;
  double lambda = 1e-9, gamma = 0.01, eta = 0.5, sigma = 4000.0;
  double epsilon = -1.0;  // -1: use half-spread at arrival
  double pov = 0.05, smile = 2.0;
  double kappa = 0.0, rho = 0.0;  // transient impact (OW resilience)
  std::string out_dir = "results";
};

Args parse_args(int argc, char** argv) {
  Args a;
  auto need = [&](int& i) -> const char* {
    if (i + 1 >= argc) {
      std::fprintf(stderr, "missing value for %s\n", argv[i]);
      std::exit(1);
    }
    return argv[++i];
  };
  for (int i = 1; i < argc; ++i) {
    const std::string f = argv[i];
    if (f == "--msg") a.msg_path = need(i);
    else if (f == "--book") a.book_path = need(i);
    else if (f == "--side") a.side = need(i);
    else if (f == "--qty") a.qty = std::atoll(need(i));
    else if (f == "--start-min") a.start_min = std::atof(need(i));
    else if (f == "--duration-min") a.duration_min = std::atof(need(i));
    else if (f == "--slices") a.slices = std::atoi(need(i));
    else if (f == "--lambda") a.lambda = std::atof(need(i));
    else if (f == "--gamma") a.gamma = std::atof(need(i));
    else if (f == "--eta") a.eta = std::atof(need(i));
    else if (f == "--sigma") a.sigma = std::atof(need(i));
    else if (f == "--epsilon") a.epsilon = std::atof(need(i));
    else if (f == "--pov") a.pov = std::atof(need(i));
    else if (f == "--smile") a.smile = std::atof(need(i));
    else if (f == "--kappa") a.kappa = std::atof(need(i));
    else if (f == "--rho") a.rho = std::atof(need(i));
    else if (f == "--out-dir") a.out_dir = need(i);
    else {
      std::fprintf(stderr, "unknown flag: %s\n", f.c_str());
      std::exit(1);
    }
  }
  if (a.msg_path.empty() || a.book_path.empty()) {
    std::fprintf(stderr, "required: --msg <message.csv> --book <orderbook.csv>\n");
    std::exit(1);
  }
  return a;
}

double bps(double ticks_shares, double x, double m0) {
  return ticks_shares / (x * m0) * 1e4;
}

}  // namespace

int main(int argc, char** argv) {
  const Args a = parse_args(argc, argv);

  try {
    const oee::LobsterDay day = oee::load_day_cached(a.msg_path, a.book_path);
    const oee::Backtester bt(day);

    const oee::Nanos open = day.messages.ts.front();
    const auto min_ns = [](double m) {
      return static_cast<oee::Nanos>(m * 60.0 * 1e9);
    };
    const oee::ParentOrder parent{
        .side = a.side == "buy" ? oee::Side::kBuy : oee::Side::kSell,
        .quantity = a.qty,
        .start = open + min_ns(a.start_min),
        .end = open + min_ns(a.start_min + a.duration_min),
        .num_slices = a.slices};

    // Epsilon default: measured half-spread at arrival (in ticks).
    oee::ExecutionSimulator probe(day, std::make_unique<oee::NoImpact>());
    const oee::ShiftedBookView arrival = probe.book_at(parent.start);
    const double epsilon =
        a.epsilon >= 0.0
            ? a.epsilon
            : static_cast<double>(arrival.best_ask() - arrival.best_bid()) / 2.0;

    const oee::ac::Params ac_params{.sigma = a.sigma, .gamma = a.gamma,
                                    .eta = a.eta, .epsilon = epsilon,
                                    .lambda = a.lambda};

    std::printf("parent: %s %lld shares over %.1f min (%d slices), "
                "start %.1f min after open\n",
                a.side.c_str(), a.qty, a.duration_min, a.slices, a.start_min);
    std::printf("params: gamma=%.4g t/sh  eta=%.4g t/(sh/min)  "
                "sigma=%.4g t/sqrt(min)  eps=%.4g t  lambda=%.3g%s\n\n",
                a.gamma, a.eta, a.sigma, epsilon, a.lambda,
                (a.gamma == 0.01 && a.eta == 0.5 && a.sigma == 4000.0)
                    ? "  [PLACEHOLDER params - run calibration]" : "");

    // The four contenders, constructed independently so one ill-posed
    // configuration (e.g. AC's eta~ <= 0 well-posedness violation on
    // tick-constrained names) skips that strategy instead of killing the
    // whole comparison.
    std::vector<std::unique_ptr<oee::IExecutionStrategy>> strategies;
    const auto try_add = [&](const char* what, auto make) {
      try {
        strategies.push_back(make());
      } catch (const std::exception& e) {
        std::fprintf(stderr, "SKIPPING %s: %s\n\n", what, e.what());
      }
    };
    try_add("TWAP", [&] {
      return std::make_unique<oee::ScheduledStrategy>(
          "TWAP", oee::twap_weights(a.slices));
    });
    try_add("VWAP", [&] {
      return std::make_unique<oee::ScheduledStrategy>(
          "VWAP", oee::vwap_weights(a.slices, a.smile));
    });
    try_add("POV", [&] { return std::make_unique<oee::PovStrategy>(a.pov); });
    try_add("AC", [&] {
      return std::make_unique<oee::ScheduledStrategy>(
          "AC", oee::ac_weights(a.slices, a.duration_min, ac_params));
    });

    std::filesystem::create_directories(a.out_dir);
    std::ofstream summary(a.out_dir + "/summary.csv");
    summary << "strategy,qty,executed,remainder,arrival_mid_ticks,"
               "terminal_mid_ticks,avg_fill_ticks,is_ticks_shares,is_bps,"
               "drift_bps,permanent_bps,execution_bps,opportunity_bps,"
               "participation\n";
    std::ofstream slices(a.out_dir + "/slices.csv");
    slices << "strategy,slice_idx,ts_sec,child,filled,vwap_ticks,"
              "mid2x_before,shift_ticks\n";

    std::printf("%-10s %9s %9s %12s %8s | %7s %7s %7s %7s | %6s\n",
                "strategy", "executed", "remaind", "avg fill $", "IS bps",
                "drift", "perm", "exec", "opp", "part%");

    for (const auto& strat : strategies) {
      const oee::BacktestResult r =
          bt.run(*strat, parent,
                 std::make_unique<oee::TransientImpact>(a.gamma, a.kappa, a.rho));
      const double x = static_cast<double>(parent.quantity);
      const double m0 = r.arrival_mid_ticks;

      std::printf("%-10s %9lld %9lld %12.4f %8.2f | %7.2f %7.2f %7.2f %7.2f "
                  "| %5.1f%%\n",
                  r.strategy.c_str(), static_cast<long long>(r.executed),
                  static_cast<long long>(r.remainder),
                  r.avg_fill_ticks() * oee::kDollarsPerTick, r.is_bps,
                  bps(r.cost_drift, x, m0), bps(r.cost_permanent, x, m0),
                  bps(r.cost_execution, x, m0), bps(r.cost_opportunity, x, m0),
                  r.participation * 100.0);

      summary << r.strategy << ',' << parent.quantity << ',' << r.executed
              << ',' << r.remainder << ',' << r.arrival_mid_ticks << ','
              << r.terminal_mid_ticks << ',' << r.avg_fill_ticks() << ','
              << r.is_ticks_shares << ',' << r.is_bps << ','
              << bps(r.cost_drift, x, m0) << ','
              << bps(r.cost_permanent, x, m0) << ','
              << bps(r.cost_execution, x, m0) << ','
              << bps(r.cost_opportunity, x, m0) << ',' << r.participation
              << '\n';
      for (const oee::SliceRecord& sr : r.slices) {
        slices << r.strategy << ',' << sr.slice_idx << ','
               << static_cast<double>(sr.ts) / 1e9 << ',' << sr.child << ','
               << sr.fill.filled << ',' << sr.fill.vwap_ticks() << ','
               << sr.fill.mid2x_before << ',' << sr.shift << '\n';
      }
    }
    std::printf("\nwrote %s/summary.csv and %s/slices.csv\n",
                a.out_dir.c_str(), a.out_dir.c_str());
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAILED: %s\n", e.what());
    return 1;
  }
}
