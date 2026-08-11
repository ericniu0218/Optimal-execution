// Ad hoc smoke test: load a real LOBSTER day and print diagnostics.
// Not part of the GoogleTest suite (real sample data is gitignored and not
// guaranteed present); this is for manual verification that the parser and
// trade-event aggregator behave sanely on real, messy data rather than only
// the synthetic fixtures in cpp/tests/.
//
// Usage: oee_smoke <message.csv> <orderbook.csv> [levels]

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "oee/data/lobster_reader.hpp"
#include "oee/data/tape_cache.hpp"
#include "oee/data/trade_events.hpp"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <message.csv> <orderbook.csv> [levels]\n",
                 argv[0]);
    return 1;
  }
  const std::string msg_path = argv[1];
  const std::string book_path = argv[2];
  const int levels = argc > 3 ? std::atoi(argv[3]) : 0;

  try {
    const oee::LobsterDay day = oee::load_day(msg_path, book_path, levels);
    const auto& m = day.messages;
    const auto& b = day.book;

    std::printf("rows                 : %zu\n", m.rows());
    std::printf("levels               : %d\n", b.levels);
    std::printf("first ts (ns)        : %lld\n",
                static_cast<long long>(m.ts.front()));
    std::printf("last ts (ns)         : %lld\n",
                static_cast<long long>(m.ts.back()));
    std::printf("span (sec)           : %.3f\n",
                static_cast<double>(m.ts.back() - m.ts.front()) / 1e9);

    std::map<int, std::size_t> type_counts;
    for (std::uint8_t t : m.type) type_counts[t]++;
    std::printf("message type counts  :\n");
    for (const auto& [type, count] : type_counts) {
      std::printf("  type %d : %zu\n", type, count);
    }

    // Sanity: no sentinel prices should have leaked through normalization.
    std::size_t leaked_sentinels = 0;
    for (const oee::PriceTicks p : b.ask_price) {
      if (p == oee::kLobsterAskSentinel) ++leaked_sentinels;
    }
    for (const oee::PriceTicks p : b.bid_price) {
      if (p == oee::kLobsterBidSentinel) ++leaked_sentinels;
    }
    std::printf("leaked sentinels     : %zu (must be 0)\n", leaked_sentinels);

    // Spread sanity on rows where both sides exist.
    std::size_t crossed = 0, both_sides = 0;
    long double spread_sum = 0;
    for (std::size_t i = 0; i < b.rows(); ++i) {
      if (b.has_best_ask(i) && b.has_best_bid(i)) {
        ++both_sides;
        const auto spread = b.best_ask(i) - b.best_bid(i);
        spread_sum += spread;
        if (spread <= 0) ++crossed;
      }
    }
    std::printf("rows with both sides : %zu / %zu\n", both_sides, b.rows());
    std::printf("crossed/locked books : %zu (should be ~0)\n", crossed);
    std::printf("mean spread (ticks)  : %.2Lf (%.4Lf dollars)\n",
                both_sides ? spread_sum / static_cast<long double>(both_sides) : 0.0L,
                (both_sides ? spread_sum / static_cast<long double>(both_sides) : 0.0L) *
                    oee::kDollarsPerTick);

    const auto events = oee::build_trade_events(m, b);
    std::size_t total_legs = 0, max_legs = 0, hidden_events = 0;
    std::size_t buyer = 0, seller = 0, unknown = 0;
    for (const auto& ev : events) {
      total_legs += static_cast<std::size_t>(ev.num_legs);
      if (static_cast<std::size_t>(ev.num_legs) > max_legs) max_legs = static_cast<std::size_t>(ev.num_legs);
      if (ev.hidden) ++hidden_events;
      if (ev.aggressor == oee::Aggressor::kBuyer) ++buyer;
      else if (ev.aggressor == oee::Aggressor::kSeller) ++seller;
      else ++unknown;
    }
    std::printf("trade events         : %zu\n", events.size());
    std::printf("  avg legs/event     : %.3f\n",
                events.empty() ? 0.0 : static_cast<double>(total_legs) / events.size());
    std::printf("  max legs in 1 event: %zu\n", max_legs);
    std::printf("  hidden events      : %zu\n", hidden_events);
    std::printf("  buyer/seller/unk   : %zu / %zu / %zu\n", buyer, seller, unknown);

    // Binary cache round-trip on REAL data: the unit tests cover synthetic
    // tapes, but a cache that silently differs from a fresh parse would
    // corrupt every downstream result, so prove equality on the real day.
    const std::string cache_path = msg_path + ".smoke.oeb";
    oee::write_tape_cache(day, cache_path, 1, 2);
    const auto reloaded = oee::read_tape_cache(cache_path, 1, 2);
    std::remove(cache_path.c_str());
    const bool cache_ok =
        reloaded && reloaded->messages.ts == m.ts &&
        reloaded->messages.type == m.type &&
        reloaded->messages.order_id == m.order_id &&
        reloaded->messages.size == m.size &&
        reloaded->messages.price == m.price &&
        reloaded->messages.dir == m.dir && reloaded->book.levels == b.levels &&
        reloaded->book.ask_price == b.ask_price &&
        reloaded->book.ask_size == b.ask_size &&
        reloaded->book.bid_price == b.bid_price &&
        reloaded->book.bid_size == b.bid_size;
    std::printf("cache round-trip     : %s\n",
                cache_ok ? "identical" : "MISMATCH");
    if (!cache_ok) return 1;

    std::printf("OK\n");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAILED: %s\n", e.what());
    return 1;
  }
}
