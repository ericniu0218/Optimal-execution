#include "oee/data/lobster_reader.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

namespace oee {
namespace {

// Read an entire file into memory in one syscall-sized gulp. LOBSTER days are
// tens to hundreds of MB; a single buffer + pointer scan is far faster than
// getline/stringstream and keeps the parse loop allocation-free.
std::string slurp(const std::string& path) {
  std::unique_ptr<std::FILE, int (*)(std::FILE*)> f(std::fopen(path.c_str(), "rb"), &std::fclose);
  if (!f) throw std::runtime_error("lobster_reader: cannot open " + path);
  std::fseek(f.get(), 0, SEEK_END);
  const long len = std::ftell(f.get());
  if (len < 0) throw std::runtime_error("lobster_reader: ftell failed on " + path);
  std::fseek(f.get(), 0, SEEK_SET);
  std::string buf(static_cast<std::size_t>(len), '\0');
  if (!buf.empty() && std::fread(buf.data(), 1, buf.size(), f.get()) != buf.size()) {
    throw std::runtime_error("lobster_reader: short read on " + path);
  }
  return buf;
}

[[noreturn]] void fail(const char* what, std::size_t line_no) {
  throw std::runtime_error("lobster_reader: " + std::string(what) + " at line " +
                           std::to_string(line_no));
}

// Parse one signed integer field; advances p past the value. Throws on
// malformed input rather than silently producing garbage.
const char* parse_i64(const char* p, const char* end, std::int64_t& out,
                      std::size_t line_no) {
  auto [ptr, ec] = std::from_chars(p, end, out);
  if (ec != std::errc()) fail("malformed integer", line_no);
  return ptr;
}

// Expect and consume a comma separator.
const char* expect_comma(const char* p, const char* end, std::size_t line_no) {
  if (p >= end || *p != ',') fail("expected ','", line_no);
  return p + 1;
}

// Parse a LOBSTER timestamp ("34200.189608186" = seconds after midnight with
// up to 9 fractional digits) into integer nanoseconds. Parsing as a double
// would lose precision (2^53 ns ≈ 104 days, fine — but the round-trip through
// decimal is not exact); integer parsing is.
const char* parse_timestamp(const char* p, const char* end, Nanos& out,
                            std::size_t line_no) {
  std::int64_t secs = 0;
  p = parse_i64(p, end, secs, line_no);
  std::int64_t frac_ns = 0;
  if (p < end && *p == '.') {
    ++p;
    int digits = 0;
    while (p < end && *p >= '0' && *p <= '9') {
      if (digits < 9) {
        frac_ns = frac_ns * 10 + (*p - '0');
        ++digits;
      }
      ++p;  // ignore precision beyond ns
    }
    while (digits < 9) {  // right-pad: ".5" means 500'000'000 ns
      frac_ns *= 10;
      ++digits;
    }
  }
  out = secs * kNanosPerSec + frac_ns;
  return p;
}

// Advance past end-of-line (\n, \r\n) or end of buffer.
const char* skip_eol(const char* p, const char* end, std::size_t line_no) {
  if (p < end && *p == '\r') ++p;
  if (p < end) {
    if (*p != '\n') fail("expected end of line", line_no);
    ++p;
  }
  return p;
}

std::size_t count_lines(const std::string& buf) {
  return static_cast<std::size_t>(std::count(buf.begin(), buf.end(), '\n')) +
         (!buf.empty() && buf.back() != '\n' ? 1 : 0);
}

}  // namespace

MessageTape read_message_file(const std::string& path) {
  const std::string buf = slurp(path);
  MessageTape tape;
  tape.reserve(count_lines(buf));

  const char* p   = buf.data();
  const char* end = buf.data() + buf.size();
  std::size_t line_no = 0;
  Nanos prev_ts = 0;

  while (p < end) {
    ++line_no;
    if (*p == '\n' || *p == '\r') {  // tolerate trailing blank line
      p = skip_eol(p, end, line_no);
      continue;
    }

    Nanos ts = 0;
    std::int64_t type = 0, order_id = 0, size = 0, price = 0, dir = 0;

    p = parse_timestamp(p, end, ts, line_no);
    p = expect_comma(p, end, line_no);
    p = parse_i64(p, end, type, line_no);
    p = expect_comma(p, end, line_no);
    p = parse_i64(p, end, order_id, line_no);
    p = expect_comma(p, end, line_no);
    p = parse_i64(p, end, size, line_no);
    p = expect_comma(p, end, line_no);
    p = parse_i64(p, end, price, line_no);
    p = expect_comma(p, end, line_no);
    p = parse_i64(p, end, dir, line_no);
    // Newer LOBSTER samples append a 7th column; skip anything up to EOL.
    while (p < end && *p != '\n' && *p != '\r') ++p;
    p = skip_eol(p, end, line_no);

    // Validation. Non-decreasing timestamps are load-bearing for everything
    // downstream (replay, sweep grouping), so a violation is fatal, not a
    // warning.
    if (ts < prev_ts) fail("decreasing timestamp", line_no);
    prev_ts = ts;
    if (type < 1 || type > 7) fail("message type out of range [1,7]", line_no);
    // Direction is only meaningful for order/execution rows; halt (7) and
    // cross (6) rows repurpose these fields per the LOBSTER spec.
    if (type <= 5 && dir != -1 && dir != 1) fail("direction not in {-1,+1}", line_no);
    if (type <= 5 && size < 0) fail("negative size", line_no);

    tape.ts.push_back(ts);
    tape.type.push_back(static_cast<std::uint8_t>(type));
    tape.order_id.push_back(order_id);
    tape.size.push_back(size);
    tape.price.push_back(price);
    tape.dir.push_back(static_cast<std::int8_t>(dir));
  }
  return tape;
}

BookTape read_orderbook_file(const std::string& path, int levels) {
  const std::string buf = slurp(path);

  // Infer level count from the first line: 4 fields per level.
  if (levels <= 0) {
    const char* p = buf.data();
    const char* end = buf.data() + buf.size();
    int fields = buf.empty() ? 0 : 1;
    while (p < end && *p != '\n') {
      if (*p == ',') ++fields;
      ++p;
    }
    if (fields == 0 || fields % 4 != 0) {
      throw std::runtime_error(
          "lobster_reader: cannot infer levels (field count " +
          std::to_string(fields) + " not a multiple of 4) in " + path);
    }
    levels = fields / 4;
  }

  BookTape tape;
  tape.levels = levels;
  const std::size_t n_rows = count_lines(buf);
  const std::size_t n_cells = n_rows * static_cast<std::size_t>(levels);
  tape.ask_price.reserve(n_cells);
  tape.ask_size.reserve(n_cells);
  tape.bid_price.reserve(n_cells);
  tape.bid_size.reserve(n_cells);

  const char* p   = buf.data();
  const char* end = buf.data() + buf.size();
  std::size_t line_no = 0;

  while (p < end) {
    ++line_no;
    if (*p == '\n' || *p == '\r') {
      p = skip_eol(p, end, line_no);
      continue;
    }
    for (int l = 0; l < levels; ++l) {
      std::int64_t apx = 0, asz = 0, bpx = 0, bsz = 0;
      p = parse_i64(p, end, apx, line_no);
      p = expect_comma(p, end, line_no);
      p = parse_i64(p, end, asz, line_no);
      p = expect_comma(p, end, line_no);
      p = parse_i64(p, end, bpx, line_no);
      p = expect_comma(p, end, line_no);
      p = parse_i64(p, end, bsz, line_no);
      if (l + 1 < levels) p = expect_comma(p, end, line_no);

      // Normalize sentinel-padded (or zero-size) levels to the canonical
      // empty marker so no downstream statistic ever sees ±9999999999.
      if (apx == kLobsterAskSentinel || asz <= 0) { apx = kNullPrice; asz = 0; }
      if (bpx == kLobsterBidSentinel || bsz <= 0) { bpx = kNullPrice; bsz = 0; }

      tape.ask_price.push_back(apx);
      tape.ask_size.push_back(asz);
      tape.bid_price.push_back(bpx);
      tape.bid_size.push_back(bsz);
    }
    // Tolerate rows wider than the requested depth: reading the top K
    // levels of a deeper file is a legitimate request (and mirrors how the
    // message reader ignores LOBSTER's optional 7th column).
    while (p < end && *p != '\n' && *p != '\r') ++p;
    p = skip_eol(p, end, line_no);
  }
  return tape;
}

LobsterDay load_day(const std::string& message_path,
                    const std::string& orderbook_path, int levels) {
  LobsterDay day;
  day.messages = read_message_file(message_path);
  day.book     = read_orderbook_file(orderbook_path, levels);
  if (day.messages.rows() != day.book.rows()) {
    throw std::runtime_error(
        "lobster_reader: row count mismatch: " + std::to_string(day.messages.rows()) +
        " messages vs " + std::to_string(day.book.rows()) + " book rows");
  }
  return day;
}

}  // namespace oee
