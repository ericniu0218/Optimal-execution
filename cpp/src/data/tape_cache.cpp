#include "oee/data/tape_cache.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>

#include "oee/data/lobster_reader.hpp"

namespace oee {
namespace {

constexpr char kMagic[8] = {'O', 'E', 'E', 'C', 'A', 'C', 'H', 'E'};
constexpr std::uint32_t kVersion = 1;

// Layout tag: any change to the element widths below invalidates every
// existing cache, and this catches it without a version bump.
constexpr std::uint32_t kLayoutTag =
    static_cast<std::uint32_t>(sizeof(Nanos) << 24 | sizeof(PriceTicks) << 16 |
                               sizeof(Qty) << 8 | sizeof(std::int64_t));

struct Header {
  char magic[8];
  std::uint32_t version;
  std::uint32_t layout_tag;
  std::int32_t levels;
  std::int32_t reserved;
  std::uint64_t rows;
  std::uint64_t payload_bytes;
  std::uint64_t src_message_bytes;
  std::uint64_t src_book_bytes;
};

using FilePtr = std::unique_ptr<std::FILE, int (*)(std::FILE*)>;

FilePtr open_file(const std::string& path, const char* mode) {
  return FilePtr(std::fopen(path.c_str(), mode), &std::fclose);
}

template <typename T>
void write_array(std::FILE* f, const std::vector<T>& v,
                 const std::string& path) {
  if (v.empty()) return;
  if (std::fwrite(v.data(), sizeof(T), v.size(), f) != v.size()) {
    throw std::runtime_error("tape_cache: short write to " + path);
  }
}

template <typename T>
bool read_array(std::FILE* f, std::vector<T>& v, std::size_t n) {
  v.resize(n);
  if (n == 0) return true;
  return std::fread(v.data(), sizeof(T), n, f) == n;
}

template <typename T>
std::uint64_t bytes_of(const std::vector<T>& v) {
  return static_cast<std::uint64_t>(v.size()) * sizeof(T);
}

std::uint64_t payload_size(const LobsterDay& day) {
  return bytes_of(day.messages.ts) + bytes_of(day.messages.type) +
         bytes_of(day.messages.order_id) + bytes_of(day.messages.size) +
         bytes_of(day.messages.price) + bytes_of(day.messages.dir) +
         bytes_of(day.book.ask_price) + bytes_of(day.book.ask_size) +
         bytes_of(day.book.bid_price) + bytes_of(day.book.bid_size);
}

std::uint64_t file_size_or_zero(const std::string& path) {
  std::error_code ec;
  const auto n = std::filesystem::file_size(path, ec);
  return ec ? 0 : static_cast<std::uint64_t>(n);
}

}  // namespace

void write_tape_cache(const LobsterDay& day, const std::string& cache_path,
                      std::uint64_t src_message_bytes,
                      std::uint64_t src_book_bytes) {
  // Write to a temporary and rename, so an interrupted write can never
  // leave a plausible-looking truncated cache in place.
  const std::string tmp = cache_path + ".tmp";
  {
    FilePtr f = open_file(tmp, "wb");
    if (!f) throw std::runtime_error("tape_cache: cannot create " + tmp);

    Header h{};
    std::memcpy(h.magic, kMagic, sizeof(kMagic));
    h.version = kVersion;
    h.layout_tag = kLayoutTag;
    h.levels = day.book.levels;
    h.rows = day.messages.rows();
    h.payload_bytes = payload_size(day);
    h.src_message_bytes = src_message_bytes;
    h.src_book_bytes = src_book_bytes;
    if (std::fwrite(&h, sizeof(h), 1, f.get()) != 1) {
      throw std::runtime_error("tape_cache: cannot write header to " + tmp);
    }

    write_array(f.get(), day.messages.ts, tmp);
    write_array(f.get(), day.messages.type, tmp);
    write_array(f.get(), day.messages.order_id, tmp);
    write_array(f.get(), day.messages.size, tmp);
    write_array(f.get(), day.messages.price, tmp);
    write_array(f.get(), day.messages.dir, tmp);
    write_array(f.get(), day.book.ask_price, tmp);
    write_array(f.get(), day.book.ask_size, tmp);
    write_array(f.get(), day.book.bid_price, tmp);
    write_array(f.get(), day.book.bid_size, tmp);
  }
  std::error_code ec;
  std::filesystem::rename(tmp, cache_path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    throw std::runtime_error("tape_cache: cannot install " + cache_path);
  }
}

std::optional<LobsterDay> read_tape_cache(const std::string& cache_path,
                                          std::uint64_t src_message_bytes,
                                          std::uint64_t src_book_bytes) {
  FilePtr f = open_file(cache_path, "rb");
  if (!f) return std::nullopt;  // no cache yet

  Header h{};
  if (std::fread(&h, sizeof(h), 1, f.get()) != 1) return std::nullopt;
  if (std::memcmp(h.magic, kMagic, sizeof(kMagic)) != 0) return std::nullopt;
  if (h.version != kVersion || h.layout_tag != kLayoutTag) return std::nullopt;
  // Stale: the CSVs changed under the cache.
  if (h.src_message_bytes != src_message_bytes ||
      h.src_book_bytes != src_book_bytes) {
    return std::nullopt;
  }
  if (h.levels <= 0) return std::nullopt;
  // Truncated: the payload is not all there.
  if (file_size_or_zero(cache_path) != sizeof(Header) + h.payload_bytes) {
    return std::nullopt;
  }

  LobsterDay day;
  const auto rows = static_cast<std::size_t>(h.rows);
  const auto cells = rows * static_cast<std::size_t>(h.levels);
  day.book.levels = h.levels;

  const bool ok =
      read_array(f.get(), day.messages.ts, rows) &&
      read_array(f.get(), day.messages.type, rows) &&
      read_array(f.get(), day.messages.order_id, rows) &&
      read_array(f.get(), day.messages.size, rows) &&
      read_array(f.get(), day.messages.price, rows) &&
      read_array(f.get(), day.messages.dir, rows) &&
      read_array(f.get(), day.book.ask_price, cells) &&
      read_array(f.get(), day.book.ask_size, cells) &&
      read_array(f.get(), day.book.bid_price, cells) &&
      read_array(f.get(), day.book.bid_size, cells);
  if (!ok) {
    // The header claimed a valid, correctly-sized cache and the read still
    // failed: that is a real I/O fault, not a cache miss.
    throw std::runtime_error("tape_cache: truncated read of " + cache_path);
  }
  return day;
}

LobsterDay load_day_cached(const std::string& message_path,
                           const std::string& orderbook_path, int levels,
                           const std::string& cache_path) {
  const std::string path =
      cache_path.empty() ? message_path + ".oeb" : cache_path;
  const std::uint64_t msg_bytes = file_size_or_zero(message_path);
  const std::uint64_t book_bytes = file_size_or_zero(orderbook_path);

  if (auto cached = read_tape_cache(path, msg_bytes, book_bytes)) {
    // A cache written at a different depth cannot serve this request.
    if (levels <= 0 || cached->book.levels == levels) return std::move(*cached);
  }

  LobsterDay day = load_day(message_path, orderbook_path, levels);
  try {
    write_tape_cache(day, path, msg_bytes, book_bytes);
  } catch (const std::exception&) {
    // A read-only or full data directory must not break the run.
  }
  return day;
}

}  // namespace oee
