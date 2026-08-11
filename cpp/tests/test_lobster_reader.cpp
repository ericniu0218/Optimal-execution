#include "oee/data/lobster_reader.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace oee {
namespace {

// Minimal synthetic day, 2 book levels. Row-aligned message/book fixtures:
// LOBSTER's book row i is the state AFTER message i.
//
//   row 0: new buy  100 @ 500.00          (book: bid 500.00x100, no ask)
//   row 1: new sell 200 @ 500.10          (ask 500.10x200)
//   row 2: new sell 150 @ 500.20          (ask2 500.20x150)
//   row 3: exec 200 @ 500.10 (sweep leg1) (ask 500.20x150)
//   row 4: exec 100 @ 500.20 (sweep leg2) (ask 500.20x50)   <- same ts as row 3
//   row 5: hidden exec 60 @ 500.15        (book unchanged)
//   row 6: delete buy 100 @ 500.00        (bid gone)
const char* kMessageCsv =
    "34200.000000001,1,1001,100,5000000,1\n"
    "34200.000000500,1,1002,200,5001000,-1\n"
    "34201.5,1,1003,150,5002000,-1\n"
    "34202.000000100,4,1002,200,5001000,-1\n"
    "34202.000000100,4,1003,100,5002000,-1\n"
    "34203.25,5,0,60,5001500,-1\n"
    "34204,3,1001,100,5000000,1\n";

const char* kOrderbookCsv =
    "9999999999,0,5000000,100,9999999999,0,-9999999999,0\n"
    "5001000,200,5000000,100,9999999999,0,-9999999999,0\n"
    "5001000,200,5000000,100,5002000,150,-9999999999,0\n"
    "5002000,150,5000000,100,9999999999,0,-9999999999,0\n"
    "5002000,50,5000000,100,9999999999,0,-9999999999,0\n"
    "5002000,50,5000000,100,9999999999,0,-9999999999,0\n"
    "5002000,50,-9999999999,0,9999999999,0,-9999999999,0\n";

class LobsterReaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::path(::testing::TempDir()) / "oee_reader_test";
    std::filesystem::create_directories(dir_);
    write(dir_ / "message.csv", kMessageCsv);
    write(dir_ / "orderbook.csv", kOrderbookCsv);
  }

  static void write(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out(p);
    out << text;
  }

  std::filesystem::path dir_;
};

TEST_F(LobsterReaderTest, ParsesMessageFields) {
  const MessageTape tape = read_message_file((dir_ / "message.csv").string());
  ASSERT_EQ(tape.rows(), 7u);

  // Nanosecond timestamp precision survives the round trip exactly.
  EXPECT_EQ(tape.ts[0], 34200 * kNanosPerSec + 1);
  EXPECT_EQ(tape.ts[1], 34200 * kNanosPerSec + 500);
  EXPECT_EQ(tape.ts[2], 34201 * kNanosPerSec + 500'000'000);  // ".5" right-pads
  EXPECT_EQ(tape.ts[6], 34204 * kNanosPerSec);                // no fraction

  EXPECT_EQ(tape.type[0], 1);
  EXPECT_EQ(tape.type[3], 4);
  EXPECT_EQ(tape.type[5], 5);
  EXPECT_EQ(tape.order_id[1], 1002);
  EXPECT_EQ(tape.size[2], 150);
  EXPECT_EQ(tape.price[4], 5002000);
  EXPECT_EQ(tape.dir[0], 1);
  EXPECT_EQ(tape.dir[3], -1);
}

TEST_F(LobsterReaderTest, ParsesBookAndNormalizesSentinels) {
  const BookTape book = read_orderbook_file((dir_ / "orderbook.csv").string());
  ASSERT_EQ(book.levels, 2);
  ASSERT_EQ(book.rows(), 7u);

  // Row 0: no ask yet — sentinel must be normalized, not leak through.
  EXPECT_FALSE(book.has_best_ask(0));
  EXPECT_EQ(book.best_ask(0), kNullPrice);
  EXPECT_EQ(book.ask_sz(0, 0), 0);
  EXPECT_EQ(book.best_bid(0), 5000000);
  EXPECT_EQ(book.bid_sz(0, 0), 100);

  // Row 2: two ask levels live.
  EXPECT_EQ(book.ask_px(2, 0), 5001000);
  EXPECT_EQ(book.ask_px(2, 1), 5002000);
  EXPECT_EQ(book.ask_sz(2, 1), 150);
  EXPECT_FALSE(book.bid_px(2, 1) == kLobsterBidSentinel);  // never leaks

  // Row 6: bid deleted.
  EXPECT_FALSE(book.has_best_bid(6));
  EXPECT_TRUE(book.has_best_ask(6));
}

TEST_F(LobsterReaderTest, ReadsTopNLevelsOfADeeperFile) {
  // Requesting fewer levels than the file carries must truncate, not throw:
  // the extra columns are simply deeper book than the caller asked for.
  const BookTape one = read_orderbook_file((dir_ / "orderbook.csv").string(), 1);
  ASSERT_EQ(one.levels, 1);
  ASSERT_EQ(one.rows(), 7u);
  const BookTape two = read_orderbook_file((dir_ / "orderbook.csv").string());
  ASSERT_EQ(two.levels, 2);
  for (std::size_t r = 0; r < one.rows(); ++r) {
    EXPECT_EQ(one.ask_px(r, 0), two.ask_px(r, 0));
    EXPECT_EQ(one.bid_sz(r, 0), two.bid_sz(r, 0));
  }
}

TEST_F(LobsterReaderTest, LoadDayCrossValidatesRowCounts) {
  const LobsterDay day =
      load_day((dir_ / "message.csv").string(), (dir_ / "orderbook.csv").string());
  EXPECT_EQ(day.messages.rows(), day.book.rows());

  // Mismatched row counts must throw, not limp along misaligned.
  write(dir_ / "short.csv", "5001000,200,5000000,100,9999999999,0,-9999999999,0\n");
  EXPECT_THROW(load_day((dir_ / "message.csv").string(),
                        (dir_ / "short.csv").string()),
               std::runtime_error);
}

TEST_F(LobsterReaderTest, RejectsDecreasingTimestamps) {
  write(dir_ / "bad_ts.csv",
        "34200.5,1,1,100,5000000,1\n"
        "34200.4,1,2,100,5000000,1\n");
  EXPECT_THROW(read_message_file((dir_ / "bad_ts.csv").string()),
               std::runtime_error);
}

TEST_F(LobsterReaderTest, RejectsMalformedRows) {
  write(dir_ / "bad_dir.csv", "34200.5,1,1,100,5000000,2\n");
  EXPECT_THROW(read_message_file((dir_ / "bad_dir.csv").string()),
               std::runtime_error);

  write(dir_ / "bad_type.csv", "34200.5,9,1,100,5000000,1\n");
  EXPECT_THROW(read_message_file((dir_ / "bad_type.csv").string()),
               std::runtime_error);

  write(dir_ / "bad_int.csv", "34200.5,1,x,100,5000000,1\n");
  EXPECT_THROW(read_message_file((dir_ / "bad_int.csv").string()),
               std::runtime_error);
}

TEST_F(LobsterReaderTest, ToleratesSeventhColumnAndTrailingNewline) {
  write(dir_ / "seven.csv", "34200.5,1,1,100,5000000,1,0\n\n");
  const MessageTape tape = read_message_file((dir_ / "seven.csv").string());
  ASSERT_EQ(tape.rows(), 1u);
  EXPECT_EQ(tape.dir[0], 1);
}

}  // namespace
}  // namespace oee
