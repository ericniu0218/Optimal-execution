// Multi-window backtest: run every strategy over rolling parent-order
// windows across the day and emit one row per (window, strategy). The
// cross-window distribution of implementation shortfall is what validates
// the Almgren-Chriss tradeoff empirically — mean cost should RISE and
// dispersion should FALL as lambda increases; a single window cannot show
// this (one path is one draw).
//
// Windows overlap (step < duration) to trade sample count against serial
// correlation; disclosed in the analysis layer.
//
// Usage:
//   oee_windows --msg <m.csv> --book <b.csv> --qty N
//               --gamma G --eta E --sigma S --epsilon F
//               [--duration-min 30] [--slices 30]
//               [--start-first-min 5] [--start-last-min 355] [--step-min 5]
//               [--pov 0.05] [--smile 2.0]
//               [--ac-lambdas 3e-11,3e-10,3e-9,3e-8]
//               [--kappa K] [--rho R]   transient impact + resilience
//               [--out results/windows.csv]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "oee/backtest/backtester.hpp"
#include "oee/data/tape_cache.hpp"
#include "oee/strategy/adaptive_ac.hpp"
#include "oee/strategy/pov.hpp"
#include "oee/strategy/scheduled.hpp"

namespace {

std::vector<double> parse_lambdas(const char* s) {
  std::vector<double> out;
  const std::string str(s);
  std::size_t pos = 0;
  while (pos < str.size()) {
    std::size_t comma = str.find(',', pos);
    if (comma == std::string::npos) comma = str.size();
    out.push_back(std::atof(str.substr(pos, comma - pos).c_str()));
    pos = comma + 1;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::string msg_path, book_path, out_path = "results/windows.csv";
  long long qty = 10000;
  double duration_min = 30, start_first = 5, start_last = 355, step_min = 5;
  int slices = 30;
  double gamma = 0, eta = 0, sigma = 0, epsilon = 0, pov = 0.05, smile = 2.0;
  double kappa = 0, rho = 0;  // transient impact (OW resilience)
  std::vector<double> ac_lambdas = {3e-11, 3e-10, 3e-9, 3e-8};
  std::vector<double> adapt_shrinks = {};  // --adapt-shrinks 0.25,0.5

  for (int i = 1; i < argc; ++i) {
    const std::string f = argv[i];
    auto need = [&]() -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", f.c_str()); std::exit(1); }
      return argv[++i];
    };
    if (f == "--msg") msg_path = need();
    else if (f == "--book") book_path = need();
    else if (f == "--out") out_path = need();
    else if (f == "--qty") qty = std::atoll(need());
    else if (f == "--duration-min") duration_min = std::atof(need());
    else if (f == "--slices") slices = std::atoi(need());
    else if (f == "--start-first-min") start_first = std::atof(need());
    else if (f == "--start-last-min") start_last = std::atof(need());
    else if (f == "--step-min") step_min = std::atof(need());
    else if (f == "--gamma") gamma = std::atof(need());
    else if (f == "--eta") eta = std::atof(need());
    else if (f == "--sigma") sigma = std::atof(need());
    else if (f == "--epsilon") epsilon = std::atof(need());
    else if (f == "--pov") pov = std::atof(need());
    else if (f == "--smile") smile = std::atof(need());
    else if (f == "--kappa") kappa = std::atof(need());
    else if (f == "--rho") rho = std::atof(need());
    else if (f == "--ac-lambdas") ac_lambdas = parse_lambdas(need());
    else if (f == "--adapt-shrinks") adapt_shrinks = parse_lambdas(need());
    else { std::fprintf(stderr, "unknown flag: %s\n", f.c_str()); std::exit(1); }
  }
  if (msg_path.empty() || book_path.empty() || eta <= 0 || sigma <= 0) {
    std::fprintf(stderr,
                 "required: --msg --book --eta --sigma (see calib.json)\n");
    return 1;
  }

  try {
    const oee::LobsterDay day = oee::load_day_cached(msg_path, book_path);
    const oee::Backtester bt(day);  // trade events aggregated once
    const oee::Nanos open = day.messages.ts.front();

    // Strategy factories: fresh instance per window (strategies are
    // stateful across slices within a run).
    using Factory = std::function<std::unique_ptr<oee::IExecutionStrategy>()>;
    std::vector<std::pair<std::string, Factory>> factories;
    factories.emplace_back("TWAP", [&] {
      return std::make_unique<oee::ScheduledStrategy>(
          "TWAP", oee::twap_weights(slices));
    });
    factories.emplace_back("VWAP", [&] {
      return std::make_unique<oee::ScheduledStrategy>(
          "VWAP", oee::vwap_weights(slices, smile));
    });
    factories.emplace_back("POV", [&] {
      return std::make_unique<oee::PovStrategy>(pov);
    });
    for (const double lam : ac_lambdas) {
      char name[48];
      std::snprintf(name, sizeof(name), "AC(%.0e)", lam);
      const oee::ac::Params p{.sigma = sigma, .gamma = gamma, .eta = eta,
                              .epsilon = epsilon, .lambda = lam};
      // Weights depend only on params, not the window: compute once.
      try {
        auto weights = oee::ac_weights(slices, duration_min, p);
        factories.emplace_back(name, [name = std::string(name), weights] {
          return std::make_unique<oee::ScheduledStrategy>(name, weights);
        });
      } catch (const std::exception& e) {
        std::fprintf(stderr, "SKIPPING %s: %s\n", name, e.what());
      }
    }

    // Adaptive AC at the same lambdas, one per shrink level, so the
    // comparison against its own static twin is like-for-like.
    for (const double lam : ac_lambdas) {
      for (const double shrink : adapt_shrinks) {
        const oee::ac::Params p{.sigma = sigma, .gamma = gamma, .eta = eta,
                                .epsilon = epsilon, .lambda = lam};
        factories.emplace_back(
            "adaptive", [p, duration_min, shrink] {
              return std::make_unique<oee::AdaptiveAcStrategy>(
                  p, duration_min, shrink);
            });
      }
    }

    std::ofstream csv(out_path);
    csv << "start_min,strategy,arrival_mid_ticks,executed,remainder,is_bps,"
           "drift_bps,permanent_bps,execution_bps,opportunity_bps,"
           "participation\n";

    int n_windows = 0, n_skipped = 0;
    for (double s0 = start_first; s0 <= start_last; s0 += step_min) {
      const oee::ParentOrder parent{
          .side = oee::Side::kSell,
          .quantity = qty,
          .start = open + static_cast<oee::Nanos>(s0 * 60e9),
          .end = open + static_cast<oee::Nanos>((s0 + duration_min) * 60e9),
          .num_slices = slices};
      bool window_ok = true;
      for (const auto& [label, make] : factories) {
        try {
          auto strat = make();
          const oee::BacktestResult r =
              bt.run(*strat, parent,
                     std::make_unique<oee::TransientImpact>(gamma, kappa, rho));
          const double x = static_cast<double>(qty);
          const double m0 = r.arrival_mid_ticks;
          const auto b = [&](double v) { return v / (x * m0) * 1e4; };
          csv << s0 << ',' << r.strategy << ',' << m0 << ',' << r.executed
              << ',' << r.remainder << ',' << r.is_bps << ','
              << b(r.cost_drift) << ',' << b(r.cost_permanent) << ','
              << b(r.cost_execution) << ',' << b(r.cost_opportunity) << ','
              << r.participation << '\n';
        } catch (const std::exception& e) {
          std::fprintf(stderr, "window %.0fmin %s skipped: %s\n", s0,
                       label.c_str(), e.what());
          window_ok = false;
        }
      }
      window_ok ? ++n_windows : ++n_skipped;
    }
    std::printf("wrote %s: %d windows complete, %d with skips, "
                "%zu strategies\n",
                out_path.c_str(), n_windows, n_skipped, factories.size());
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAILED: %s\n", e.what());
    return 1;
  }
}
