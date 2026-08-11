#include "oee/data/tape_cache.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace oee {
namespace {

LobsterDay sample_day(int rows = 5, int levels = 2) {
  LobsterDay day;
  day.book.levels = levels;
  for (int i = 0; i < rows; ++i) {
    day.messages.ts.push_back(34200 * kNanosPerSec + i);
    day.messages.type.push_back(static_cast<std::uint8_t>(1 + (i % 5)));
    day.messages.order_id.push_back(1000 + i);
    day.messages.size.push_back(10 * (i + 1));
    day.messages.price.push_back(5000000 + 100 * i);
    day.messages.dir.push_back(i % 2 == 0 ? 1 : -1);
    for (int l = 0; l < levels; ++l) {
      day.book.ask_price.push_back(5001000 + 100 * l);
      day.book.ask_size.push_back(100 + l);
      day.book.bid_price.push_back(5000000 - 100 * l);
      day.book.bid_size.push_back(200 + l);
    }
  }
  return day;
}

void expect_days_equal(const LobsterDay& a, const LobsterDay& b) {
  EXPECT_EQ(a.messages.ts, b.messages.ts);
  EXPECT_EQ(a.messages.type, b.messages.type);
  EXPECT_EQ(a.messages.order_id, b.messages.order_id);
  EXPECT_EQ(a.messages.size, b.messages.size);
  EXPECT_EQ(a.messages.price, b.messages.price);
  EXPECT_EQ(a.messages.dir, b.messages.dir);
  EXPECT_EQ(a.book.levels, b.book.levels);
  EXPECT_EQ(a.book.ask_price, b.book.ask_price);
  EXPECT_EQ(a.book.ask_size, b.book.ask_size);
  EXPECT_EQ(a.book.bid_price, b.book.bid_price);
  EXPECT_EQ(a.book.bid_size, b.book.bid_size);
}

class TapeCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::path(::testing::TempDir()) / "oee_cache_test";
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    cache_ = (dir_ / "day.oeb").string();
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  std::filesystem::path dir_;
  std::string cache_;
};

TEST_F(TapeCacheTest, RoundTripIsExact) {
  const LobsterDay day = sample_day();
  write_tape_cache(day, cache_, 111, 222);
  const auto back = read_tape_cache(cache_, 111, 222);
  ASSERT_TRUE(back.has_value());
  expect_days_equal(day, *back);
}

TEST_F(TapeCacheTest, MissingCacheIsAMiss) {
  EXPECT_FALSE(read_tape_cache((dir_ / "nope.oeb").string(), 1, 2));
}

TEST_F(TapeCacheTest, StaleSourceSizesAreAMiss) {
  write_tape_cache(sample_day(), cache_, 111, 222);
  EXPECT_FALSE(read_tape_cache(cache_, 999, 222));  // message file changed
  EXPECT_FALSE(read_tape_cache(cache_, 111, 999));  // book file changed
  EXPECT_TRUE(read_tape_cache(cache_, 111, 222));
}

TEST_F(TapeCacheTest, ForeignOrCorruptHeaderIsAMiss) {
  // Not our file at all.
  {
    std::ofstream out(cache_, std::ios::binary);
    out << "this is definitely not a tape cache, but it is long enough";
  }
  EXPECT_FALSE(read_tape_cache(cache_, 111, 222));

  // Right magic, wrong version byte.
  write_tape_cache(sample_day(), cache_, 111, 222);
  {
    std::fstream f(cache_, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(8);  // version follows the 8-byte magic
    const std::uint32_t bogus = 0xDEADBEEF;
    f.write(reinterpret_cast<const char*>(&bogus), sizeof(bogus));
  }
  EXPECT_FALSE(read_tape_cache(cache_, 111, 222));
}

TEST_F(TapeCacheTest, TruncatedPayloadIsAMissNotAMisread) {
  // The dangerous failure: a cache whose header is intact but whose data
  // was cut short. Must be rejected, never partially believed.
  write_tape_cache(sample_day(20), cache_, 111, 222);
  const auto full = std::filesystem::file_size(cache_);
  std::filesystem::resize_file(cache_, full - 64);
  EXPECT_FALSE(read_tape_cache(cache_, 111, 222));
}

TEST_F(TapeCacheTest, EmptyDayRoundTrips) {
  LobsterDay empty;
  empty.book.levels = 10;
  write_tape_cache(empty, cache_, 0, 0);
  const auto back = read_tape_cache(cache_, 0, 0);
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(back->messages.rows(), 0u);
  EXPECT_EQ(back->book.levels, 10);
}

TEST_F(TapeCacheTest, LoadDayCachedPopulatesThenReusesCache) {
  // Write real CSVs, load twice, and confirm the second load comes from a
  // cache that produces an identical day.
  const std::string msg = (dir_ / "m.csv").string();
  const std::string book = (dir_ / "b.csv").string();
  {
    std::ofstream m(msg);
    m << "34200.5,1,1,100,5000000,1\n34201.25,4,1,40,5000000,1\n";
    std::ofstream b(book);
    b << "5001000,200,5000000,100\n5001000,200,5000000,60\n";
  }
  const std::string cache = msg + ".oeb";
  ASSERT_FALSE(std::filesystem::exists(cache));

  const LobsterDay first = load_day_cached(msg, book);
  ASSERT_TRUE(std::filesystem::exists(cache));
  const LobsterDay second = load_day_cached(msg, book);
  expect_days_equal(first, second);
  EXPECT_EQ(second.messages.rows(), 2u);
  EXPECT_EQ(second.book.levels, 1);

  // Rewriting the source invalidates the cache; the reload must reflect
  // the NEW content, not the stale image.
  {
    std::ofstream m(msg);
    m << "34200.5,1,1,100,5000000,1\n34201.25,4,1,40,5000000,1\n"
         "34202.0,3,1,60,5000000,1\n";
    std::ofstream b(book);
    b << "5001000,200,5000000,100\n5001000,200,5000000,60\n"
         "5001000,200,-9999999999,0\n";
  }
  const LobsterDay third = load_day_cached(msg, book);
  EXPECT_EQ(third.messages.rows(), 3u);
  EXPECT_FALSE(third.book.has_best_bid(2));
}

TEST_F(TapeCacheTest, LevelMismatchFallsBackToParse) {
  const std::string msg = (dir_ / "m.csv").string();
  const std::string book = (dir_ / "b.csv").string();
  {
    std::ofstream m(msg);
    m << "34200.5,1,1,100,5000000,1\n";
    std::ofstream b(book);
    b << "5001000,200,5000000,100,5002000,50,4999000,20\n";
  }
  // First load infers 2 levels and caches it.
  EXPECT_EQ(load_day_cached(msg, book).book.levels, 2);
  // Requesting a different depth must not be served from that cache.
  EXPECT_EQ(load_day_cached(msg, book, 1).book.levels, 1);
}

}  // namespace
}  // namespace oee
