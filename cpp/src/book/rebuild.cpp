#include "oee/book/rebuild.hpp"

#include <algorithm>

namespace oee {
namespace {

// Outcome of comparing one side's rebuilt levels against one snapshot row.
struct SideResult {
  int surfaced = 0;
  bool hard = false;
};

std::vector<RebuiltBook::Level> snapshot_side(const BookTape& book,
                                              std::size_t row,
                                              std::int8_t dir, int levels) {
  std::vector<RebuiltBook::Level> out;
  for (int l = 0; l < levels; ++l) {
    const PriceTicks px = dir == 1 ? book.bid_px(row, l) : book.ask_px(row, l);
    const Qty sz = dir == 1 ? book.bid_sz(row, l) : book.ask_sz(row, l);
    if (px == kNullPrice) break;  // occupied levels are contiguous
    out.emplace_back(px, sz);
  }
  return out;
}

}  // namespace

void RebuiltBook::add(std::int8_t dir, PriceTicks px, Qty qty) {
  if (dir == 1) bids_[px] += qty;
  else asks_[px] += qty;
}

Qty RebuiltBook::reduce(std::int8_t dir, PriceTicks px, Qty qty) {
  auto reduce_in = [&](auto& side) -> Qty {
    auto it = side.find(px);
    if (it == side.end()) return qty;  // nothing tracked: fully clamped
    const Qty removed = std::min(it->second, qty);
    it->second -= removed;
    if (it->second == 0) side.erase(it);
    return qty - removed;
  };
  return dir == 1 ? reduce_in(bids_) : reduce_in(asks_);
}

void RebuiltBook::set_level(std::int8_t dir, PriceTicks px, Qty qty) {
  if (dir == 1) bids_[px] = qty;
  else asks_[px] = qty;
}

std::vector<RebuiltBook::Level> RebuiltBook::top(std::int8_t dir,
                                                 int k) const {
  std::vector<Level> out;
  out.reserve(static_cast<std::size_t>(k));
  if (dir == 1) {
    for (const auto& [px, qty] : bids_) {
      if (static_cast<int>(out.size()) == k) break;
      out.emplace_back(px, qty);
    }
  } else {
    for (const auto& [px, qty] : asks_) {
      if (static_cast<int>(out.size()) == k) break;
      out.emplace_back(px, qty);
    }
  }
  return out;
}

void RebuiltBook::resync(std::int8_t dir,
                         const std::vector<Level>& levels) {
  if (dir == 1) bids_.clear();
  else asks_.clear();
  for (const auto& [px, qty] : levels) set_level(dir, px, qty);
}

std::size_t RebuiltBook::prune_beyond(std::int8_t dir,
                                      PriceTicks worst_visible) {
  // Both maps are ordered best-first, so "strictly worse than
  // worst_visible" is the tail past upper_bound(worst_visible).
  auto prune_in = [worst_visible](auto& side) {
    auto it = side.upper_bound(worst_visible);
    const auto n = static_cast<std::size_t>(std::distance(it, side.end()));
    side.erase(it, side.end());
    return n;
  };
  return dir == 1 ? prune_in(bids_) : prune_in(asks_);
}

RebuildStats validate_rebuild(const LobsterDay& day, int check_levels) {
  const auto& m = day.messages;
  const int L = check_levels > 0 ? std::min(check_levels, day.book.levels)
                                 : day.book.levels;

  RebuildStats st;
  if (m.rows() == 0) return st;

  RebuiltBook book;
  // Seed from the first snapshot: the message file cannot describe orders
  // resting from before the file window, but the first book row shows
  // their aggregate. Validation runs from row 1.
  for (const std::int8_t dir : {std::int8_t{1}, std::int8_t{-1}}) {
    for (const auto& [px, qty] : snapshot_side(day.book, 0, dir, L)) {
      book.set_level(dir, px, qty);
    }
  }

  // Compare one side against the snapshot; reseed benign gaps in place.
  const auto compare_side = [&](std::size_t row, std::int8_t dir) {
    SideResult res;
    const auto snap = snapshot_side(day.book, row, dir, L);
    const bool snap_truncated = static_cast<int>(snap.size()) == L;
    // Fetch one extra rebuilt level so "we track more levels than the
    // snapshot shows" is detectable when the snapshot is NOT truncated.
    const auto mine = book.top(dir, L + 1);

    const auto better = [dir](PriceTicks a, PriceTicks b) {
      return dir == 1 ? a > b : a < b;
    };

    std::size_t mi = 0;
    for (const auto& [spx, ssz] : snap) {
      if (mi >= mine.size()) {
        // Snapshot has a level we never saw: deep liquidity surfacing.
        book.set_level(dir, spx, ssz);
        ++res.surfaced;
        continue;
      }
      const auto [mpx, msz] = mine[mi];
      if (mpx == spx) {
        if (msz == ssz) {
          ++mi;
        } else if (msz < ssz) {
          // Unseen liquidity joined a price we track (e.g. a pre-file
          // order deeper than the visible range scrolled in): benign.
          book.set_level(dir, spx, ssz);
          ++res.surfaced;
          ++mi;
        } else {
          res.hard = true;  // we claim more than the exchange shows
          break;
        }
      } else if (better(mpx, spx)) {
        res.hard = true;  // phantom level better than the snapshot's best
        break;
      } else {
        // Snapshot level absent from the rebuild: surfacing.
        book.set_level(dir, spx, ssz);
        ++res.surfaced;
      }
    }
    if (!res.hard && !snap_truncated && mi < mine.size()) {
      // Snapshot says the book ends here, yet we track deeper levels:
      // phantom liquidity.
      res.hard = true;
    }
    if (res.hard) {
      book.resync(dir, snap);  // stop a single fault from cascading
    } else if (snap_truncated) {
      // The window is full: levels beyond its worst visible price are
      // leaving observability and their events will be omitted from the
      // file. Drop them now; re-entry is handled as surfacing.
      st.pruned_levels += book.prune_beyond(dir, snap.back().first);
    }
    return res;
  };

  for (std::size_t i = 1; i < m.rows(); ++i) {
    switch (static_cast<MessageType>(m.type[i])) {
      case MessageType::kNewOrder:
        book.add(m.dir[i], m.price[i], m.size[i]);
        break;
      case MessageType::kPartialCancel:
      case MessageType::kDelete:
      case MessageType::kExecuteVisible:
        if (book.reduce(m.dir[i], m.price[i], m.size[i]) > 0) {
          ++st.clamped_deltas;
        }
        break;
      case MessageType::kExecuteHidden:
      case MessageType::kCross:
      case MessageType::kHalt:
        break;  // no visible-book effect
    }

    const SideResult b = compare_side(i, 1);
    const SideResult a = compare_side(i, -1);
    ++st.rows;
    st.surfaced_levels +=
        static_cast<std::size_t>(b.surfaced + a.surfaced);
    if (b.hard || a.hard) {
      ++st.hard_rows;
      if (st.first_hard_row < 0) {
        st.first_hard_row = static_cast<std::ptrdiff_t>(i);
      }
    } else if (b.surfaced + a.surfaced > 0) {
      ++st.surfaced_rows;
    } else {
      ++st.exact_rows;
    }
  }
  return st;
}

}  // namespace oee
