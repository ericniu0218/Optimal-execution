// Sweep lambda and emit the analytic Almgren-Chriss efficient frontier:
// E[IS] vs sqrt(Var[IS]) for the optimal trajectory at each risk aversion.
// This is the model-implied frontier (planner units); the realized
// single-path IS from backtests overlays it in the analysis layer.
//
// Usage:
//   oee_frontier --qty N --duration-min T --slices N
//                --gamma G --eta E --sigma S --epsilon F
//                [--arrival-mid M] [--lambda-min 1e-9] [--lambda-max 1e-4]
//                [--points 25] [--out frontier.csv]
//
// Output columns: lambda, kappa_per_min, expected_cost_ticks_shares,
// std_ticks_shares, expected_cost_bps, std_bps (bps columns need
// --arrival-mid, else 0).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "oee/ac/solver.hpp"

int main(int argc, char** argv) {
  double qty = 20000, duration_min = 60, arrival_mid = 0;
  int slices = 60, points = 25;
  double gamma = 0, eta = 0, sigma = 0, epsilon = 0;
  double lambda_min = 1e-9, lambda_max = 1e-4;
  std::string out = "results/frontier.csv";

  for (int i = 1; i < argc; ++i) {
    const std::string f = argv[i];
    auto need = [&]() -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", f.c_str()); std::exit(1); }
      return argv[++i];
    };
    if (f == "--qty") qty = std::atof(need());
    else if (f == "--duration-min") duration_min = std::atof(need());
    else if (f == "--slices") slices = std::atoi(need());
    else if (f == "--gamma") gamma = std::atof(need());
    else if (f == "--eta") eta = std::atof(need());
    else if (f == "--sigma") sigma = std::atof(need());
    else if (f == "--epsilon") epsilon = std::atof(need());
    else if (f == "--arrival-mid") arrival_mid = std::atof(need());
    else if (f == "--lambda-min") lambda_min = std::atof(need());
    else if (f == "--lambda-max") lambda_max = std::atof(need());
    else if (f == "--points") points = std::atoi(need());
    else if (f == "--out") out = need();
    else { std::fprintf(stderr, "unknown flag: %s\n", f.c_str()); std::exit(1); }
  }
  if (eta <= 0 || sigma <= 0) {
    std::fprintf(stderr, "required: --eta and --sigma (> 0); see calib.json\n");
    return 1;
  }

  std::ofstream csv(out);
  csv << "lambda,kappa_per_min,expected_cost_ticks_shares,std_ticks_shares,"
         "expected_cost_bps,std_bps\n";

  const double notional = qty * arrival_mid;  // ticks*shares
  const double log_lo = std::log(lambda_min);
  const double log_hi = std::log(lambda_max);
  int written = 0;
  for (int i = 0; i < points; ++i) {
    const double lambda =
        std::exp(log_lo + (log_hi - log_lo) * i / (points - 1));
    const oee::ac::Params p{.sigma = sigma, .gamma = gamma, .eta = eta,
                            .epsilon = epsilon, .lambda = lambda};
    try {
      const oee::ac::Solution sol =
          oee::ac::solve(qty, duration_min, slices, p);
      const double sd = std::sqrt(sol.variance);
      csv << lambda << ',' << sol.kappa << ',' << sol.expected_cost << ','
          << sd << ','
          << (notional > 0 ? sol.expected_cost / notional * 1e4 : 0.0) << ','
          << (notional > 0 ? sd / notional * 1e4 : 0.0) << '\n';
      ++written;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "lambda=%g skipped: %s\n", lambda, e.what());
    }
  }
  std::printf("wrote %s (%d/%d lambdas)\n", out.c_str(), written, points);
  return written > 0 ? 0 : 1;
}
