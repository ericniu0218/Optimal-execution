#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "oee/core/types.hpp"

namespace oee {

// ---------------------------------------------------------------------------
// Structure-of-arrays tapes for one LOBSTER trading day.
//
// SoA rather than vector<Message>: the replay loop touches one or two fields
// per message (usually ts + type), and parallel arrays keep those accesses
// dense in cache. A full-depth day for a liquid name is millions of rows.
// ---------------------------------------------------------------------------

// One row per LOBSTER message-file line.
struct MessageTape {
  std::vector<Nanos>        ts;        // nanoseconds after midnight
  std::vector<std::uint8_t> type;      // raw MessageType value (1..7)
  std::vector<std::int64_t> order_id;
  std::vector<Qty>          size;
  std::vector<PriceTicks>   price;
  std::vector<std::int8_t>  dir;       // resting-order direction: -1 sell, +1 buy

  std::size_t rows() const { return ts.size(); }

  void reserve(std::size_t n) {
    ts.reserve(n);
    type.reserve(n);
    order_id.reserve(n);
    size.reserve(n);
    price.reserve(n);
    dir.reserve(n);
  }
};

// One row per LOBSTER orderbook-file line: the book state AFTER the
// corresponding message. Levels are flattened row-major:
// ask_price[row * levels + l] is ask level l (0-based, l=0 is best).
// Unoccupied levels are normalized to price = kNullPrice, size = 0.
struct BookTape {
  int levels = 0;
  std::vector<PriceTicks> ask_price;
  std::vector<Qty>        ask_size;
  std::vector<PriceTicks> bid_price;
  std::vector<Qty>        bid_size;

  std::size_t rows() const {
    return levels > 0 ? ask_price.size() / static_cast<std::size_t>(levels) : 0;
  }

  PriceTicks ask_px(std::size_t row, int level) const {
    return ask_price[row * static_cast<std::size_t>(levels) + static_cast<std::size_t>(level)];
  }
  Qty ask_sz(std::size_t row, int level) const {
    return ask_size[row * static_cast<std::size_t>(levels) + static_cast<std::size_t>(level)];
  }
  PriceTicks bid_px(std::size_t row, int level) const {
    return bid_price[row * static_cast<std::size_t>(levels) + static_cast<std::size_t>(level)];
  }
  Qty bid_sz(std::size_t row, int level) const {
    return bid_size[row * static_cast<std::size_t>(levels) + static_cast<std::size_t>(level)];
  }

  PriceTicks best_ask(std::size_t row) const { return ask_px(row, 0); }
  PriceTicks best_bid(std::size_t row) const { return bid_px(row, 0); }
  bool has_best_ask(std::size_t row) const { return best_ask(row) != kNullPrice; }
  bool has_best_bid(std::size_t row) const { return best_bid(row) != kNullPrice; }

  // 2x the mid price, kept in integer ticks to avoid a float in comparisons
  // (bid + ask is exact; bid/2 + ask/2 is not). Callers needing the actual
  // mid divide by 2.0 at the analysis boundary.
  PriceTicks mid2x(std::size_t row) const { return best_ask(row) + best_bid(row); }
};

// A fully loaded day: message tape + book tape, row-aligned by construction
// (LOBSTER emits exactly one orderbook row per message row).
struct LobsterDay {
  MessageTape messages;
  BookTape    book;
};

}  // namespace oee
