#pragma once

#include <cstdint>

namespace oee {

// ---------------------------------------------------------------------------
// Core value types.
//
// LOBSTER encodes prices as integers in units of 1/10000 dollar (0.01 cent).
// We keep prices on that integer lattice end-to-end; conversion to floating
// point happens only at the analysis boundary (calibration / plotting).
// Rationale: doubles on a discrete price grid invite tick-alignment bugs when
// walking book levels, and integer comparisons are exact.
// ---------------------------------------------------------------------------

using PriceTicks = std::int64_t;  // price in 1e-4 dollars
using Qty        = std::int64_t;  // shares
using Nanos      = std::int64_t;  // nanoseconds after midnight

inline constexpr double kDollarsPerTick = 1e-4;
inline constexpr Nanos  kNanosPerSec    = 1'000'000'000;

// LOBSTER pads unoccupied book levels with sentinel prices (and size 0):
//   asks: 9999999999, bids: -9999999999.
// Left unfiltered these look like real numbers and poison every depth/spread
// statistic, so the parser normalizes them to kNullPrice / size 0.
inline constexpr PriceTicks kLobsterAskSentinel = 9999999999LL;
inline constexpr PriceTicks kLobsterBidSentinel = -9999999999LL;
inline constexpr PriceTicks kNullPrice          = 0;

// LOBSTER message-file event types.
enum class MessageType : std::uint8_t {
  kNewOrder       = 1,  // submission of a new visible limit order
  kPartialCancel  = 2,  // partial cancellation
  kDelete         = 3,  // full deletion of a visible limit order
  kExecuteVisible = 4,  // execution of a visible limit order
  kExecuteHidden  = 5,  // execution of a hidden order (direction unreliable)
  kCross          = 6,  // auction / cross trade
  kHalt           = 7,  // trading halt indicator
};

// Direction of a *resting* limit order, as encoded in the message file.
enum class BookSide : std::int8_t { kSell = -1, kBuy = 1 };

// Aggressor side of a trade (who initiated it). For type-4 executions this is
// the opposite of the resting order's direction: direction = -1 means a
// resting SELL was hit, i.e. the aggressor was a BUYER. Getting this backwards
// flips the sign of the permanent-impact coefficient downstream.
enum class Aggressor : std::int8_t { kSeller = -1, kUnknown = 0, kBuyer = 1 };

// Side of OUR order flow (the strategy's child orders). Kept distinct from
// BookSide to avoid ever confusing "direction of the resting order in the
// feed" with "direction we are trading".
enum class Side : std::int8_t { kSell = -1, kBuy = 1 };

}  // namespace oee
