#pragma once

#include <string>

#include "oee/data/message_tape.hpp"

namespace oee {

// ---------------------------------------------------------------------------
// LOBSTER CSV readers.
//
// File formats (see lobsterdata.com/info/DataStructure.php):
//
//   message file : time,type,order_id,size,price,direction[,extra]
//     - time      seconds after midnight with decimal fraction (ns resolution)
//     - type      1..7 (MessageType)
//     - price     integer, 1e-4 dollars
//     - direction direction of the RESTING limit order: -1 sell, +1 buy
//       (some samples carry a 7th column; it is ignored)
//
//   orderbook file : ask_p1,ask_sz1,bid_p1,bid_sz1,ask_p2,... (4 fields/level)
//     - the book state AFTER the corresponding message-file row
//     - unoccupied levels padded with sentinel prices (normalized on load)
//
// Parsing uses std::from_chars over a single slurped buffer — no iostreams,
// no per-field allocation. Malformed input throws std::runtime_error with the
// 1-based line number.
// ---------------------------------------------------------------------------

// Parse a message file. Enforces non-decreasing timestamps and validates
// type/direction ranges (direction only for types 1..5; halt and cross rows
// use these fields for other purposes per the LOBSTER spec).
MessageTape read_message_file(const std::string& path);

// Parse an orderbook file. `levels <= 0` infers the level count from the
// first line's field count (must be a multiple of 4); a positive `levels`
// reads the top N levels and ignores any deeper columns present.
BookTape read_orderbook_file(const std::string& path, int levels = 0);

// Load and cross-validate a full day: message and orderbook files must have
// identical row counts (LOBSTER guarantees one book row per message).
LobsterDay load_day(const std::string& message_path,
                    const std::string& orderbook_path,
                    int levels = 0);

}  // namespace oee
