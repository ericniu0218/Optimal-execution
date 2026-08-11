#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "oee/data/message_tape.hpp"

namespace oee {

// ---------------------------------------------------------------------------
// Binary tape cache.
//
// CSV parsing dominates the cost of every tool here, and the analysis loop
// re-reads the same day constantly (every lambda, every window sweep). The
// cache is a direct image of the in-memory SoA tapes: loading it is a
// sequence of bulk reads instead of a per-field integer parse.
//
// Deliberate non-goals:
//   - NOT portable. Fixed-width little-endian host layout; a cache written
//     on one machine is not promised to another. It is a derived artifact,
//     regenerated on demand — the header records the layout it was written
//     with and refuses anything else rather than silently misreading.
//   - NOT smaller than the CSV. Prices/sizes are narrow in text and 8 bytes
//     in memory, so the cache is typically ~30% LARGER on disk. The win is
//     parse time, not space; that trade is the whole point.
//   - NOT checksummed. Staleness and truncation are caught by recording the
//     source CSV byte sizes and the expected payload length; hashing 200 MB
//     on every load would eat the win it exists to deliver.
// ---------------------------------------------------------------------------

// Write `day` to `cache_path`, stamping the source sizes for staleness
// detection. Throws std::runtime_error on I/O failure.
void write_tape_cache(const LobsterDay& day, const std::string& cache_path,
                      std::uint64_t src_message_bytes,
                      std::uint64_t src_book_bytes);

// Read a cache written for exactly these source sizes. Returns nullopt (not
// an exception) when the cache is absent, stale, or written by a different
// layout/version — all of which simply mean "reparse". Throws only when the
// file exists, claims to be a valid cache, and then fails to deliver one.
std::optional<LobsterDay> read_tape_cache(const std::string& cache_path,
                                          std::uint64_t src_message_bytes,
                                          std::uint64_t src_book_bytes);

// Load a day, using (and populating) a binary cache beside the message file
// at "<message_path>.oeb" unless `cache_path` overrides it. A cache write
// failure is non-fatal: the day is still returned.
LobsterDay load_day_cached(const std::string& message_path,
                           const std::string& orderbook_path,
                           int levels = 0,
                           const std::string& cache_path = "");

}  // namespace oee
