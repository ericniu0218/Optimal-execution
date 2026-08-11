// Validate message-driven book reconstruction against LOBSTER's own
// snapshots for a full day. See oee/book/rebuild.hpp for the exact/
// surfaced/hard taxonomy; the bar is zero hard rows.
//
// Usage: oee_rebuild --msg <message.csv> --book <orderbook.csv> [--levels K]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "oee/book/rebuild.hpp"
#include "oee/data/tape_cache.hpp"

int main(int argc, char** argv) {
  std::string msg_path, book_path;
  int levels = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string f = argv[i];
    auto need = [&]() -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", f.c_str()); std::exit(1); }
      return argv[++i];
    };
    if (f == "--msg") msg_path = need();
    else if (f == "--book") book_path = need();
    else if (f == "--levels") levels = std::atoi(need());
    else { std::fprintf(stderr, "unknown flag: %s\n", f.c_str()); std::exit(1); }
  }
  if (msg_path.empty() || book_path.empty()) {
    std::fprintf(stderr, "required: --msg <message.csv> --book <orderbook.csv>\n");
    return 1;
  }

  try {
    const oee::LobsterDay day = oee::load_day_cached(msg_path, book_path);

    const auto t0 = std::chrono::steady_clock::now();
    const oee::RebuildStats st = oee::validate_rebuild(day, levels);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();

    const double pct = st.rows ? 100.0 * static_cast<double>(st.exact_rows) /
                                     static_cast<double>(st.rows)
                               : 0.0;
    std::printf("rows validated   : %zu (in %.0f ms, %.1fM rows/s)\n",
                st.rows, ms, static_cast<double>(st.rows) / ms / 1e3);
    std::printf("exact            : %zu (%.4f%%)\n", st.exact_rows, pct);
    std::printf("surfaced (benign): %zu rows, %zu levels reseeded\n",
                st.surfaced_rows, st.surfaced_levels);
    std::printf("pruned levels    : %zu (left the visible window)\n",
                st.pruned_levels);
    std::printf("clamped deltas   : %zu\n", st.clamped_deltas);
    std::printf("HARD mismatches  : %zu", st.hard_rows);
    if (st.first_hard_row >= 0) {
      std::printf("  (first at row %td)", st.first_hard_row);
    }
    std::printf("\n%s\n", st.clean() ? "CLEAN" : "FAILED");
    return st.clean() ? 0 : 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAILED: %s\n", e.what());
    return 1;
  }
}
