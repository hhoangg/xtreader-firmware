// Pins the marker that stops a failed progress push from being silently
// forgotten: its on-disk round trip, every way the file can be unusable, and
// the baseline decision EpubReaderActivity::hasUnsyncedProgress() got wrong.
//
// The decision cases are the point. Before the fix the baseline was always the
// position the book resumed at, so "marker is older than where the book
// resumed" -- a push that failed last session -- reported "nothing to send"
// and the position was lost for good.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "activities/reader/SyncedPositionMarker.h"

namespace {

using SyncedPositionMarker::Position;

// Each test gets its own directory, so a stale marker from an earlier test
// cannot be mistaken for the one under test.
class SyncedPositionMarkerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
    // Named after the test, not a pid: ctest -j runs each case as its own
    // process, and distinct names are what keeps those from colliding.
    cachePath =
        (std::filesystem::temp_directory_path() / ("crosspoint_synced_position_" + std::string(info->name()))).string();
    std::filesystem::remove_all(cachePath);
    std::filesystem::create_directories(cachePath);
  }

  void TearDown() override { std::filesystem::remove_all(cachePath); }

  void writeRawMarker(const std::vector<uint8_t>& bytes) const {
    std::ofstream out(SyncedPositionMarker::markerPath(cachePath), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

  std::string cachePath;
};

TEST_F(SyncedPositionMarkerTest, RoundTripsAPosition) {
  ASSERT_TRUE(SyncedPositionMarker::save(cachePath, Position{12, 340}));

  const auto loaded = SyncedPositionMarker::load(cachePath);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->spineIndex, 12);
  EXPECT_EQ(loaded->pageNumber, 340);
}

TEST_F(SyncedPositionMarkerTest, RoundTripsTheFirstPageOfTheFirstSpineItem) {
  // Zero is a real position, not "unset": the marker's absent state is the
  // missing file, never a zero payload.
  ASSERT_TRUE(SyncedPositionMarker::save(cachePath, Position{0, 0}));

  const auto loaded = SyncedPositionMarker::load(cachePath);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->spineIndex, 0);
  EXPECT_EQ(loaded->pageNumber, 0);
}

TEST_F(SyncedPositionMarkerTest, WritesExactlyEightBytes) {
  ASSERT_TRUE(SyncedPositionMarker::save(cachePath, Position{1, 2}));
  EXPECT_EQ(std::filesystem::file_size(SyncedPositionMarker::markerPath(cachePath)), 8u);
}

TEST_F(SyncedPositionMarkerTest, MissingFileReadsAsNothingEverPushed) {
  EXPECT_FALSE(SyncedPositionMarker::load(cachePath).has_value());
}

TEST_F(SyncedPositionMarkerTest, TruncatedFileReadsAsNothingEverPushedNotAGarbagePosition) {
  // A torn write: the spine index landed, the page number did not. This must
  // not read back as page 0 of that spine item.
  writeRawMarker({0x0C, 0x00, 0x00, 0x00, 0x54});

  EXPECT_FALSE(SyncedPositionMarker::load(cachePath).has_value());
}

TEST_F(SyncedPositionMarkerTest, EmptyFileReadsAsNothingEverPushed) {
  writeRawMarker({});

  EXPECT_FALSE(SyncedPositionMarker::load(cachePath).has_value());
}

TEST_F(SyncedPositionMarkerTest, NegativeFieldsReadAsNothingEverPushed) {
  // save() cannot produce these, so they are corruption; a garbage baseline
  // would be worse than no baseline.
  writeRawMarker({0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0x00, 0x00, 0x00});

  EXPECT_FALSE(SyncedPositionMarker::load(cachePath).has_value());
}

TEST_F(SyncedPositionMarkerTest, RefusesToSaveAnUnsetPosition) {
  EXPECT_FALSE(SyncedPositionMarker::save(cachePath, Position{}));
  EXPECT_FALSE(SyncedPositionMarker::save(cachePath, Position{3, -1}));
  EXPECT_FALSE(std::filesystem::exists(SyncedPositionMarker::markerPath(cachePath)));
}

TEST_F(SyncedPositionMarkerTest, RefusesToSaveWithoutACachePath) {
  EXPECT_FALSE(SyncedPositionMarker::save("", Position{1, 2}));
}

TEST_F(SyncedPositionMarkerTest, OverwritesAnEarlierMarker) {
  ASSERT_TRUE(SyncedPositionMarker::save(cachePath, Position{2, 9}));
  ASSERT_TRUE(SyncedPositionMarker::save(cachePath, Position{2, 10}));

  const auto loaded = SyncedPositionMarker::load(cachePath);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->pageNumber, 10);
}

// --- the decision itself ---------------------------------------------------

TEST(SyncedPositionBaseline, MarkerAbsentFallsBackToTheResumePosition) {
  // A book nobody has ever pushed must behave exactly as it did before the
  // marker existed: only real movement counts. Reporting "unsynced" here
  // would upload the resume position on every open-and-sleep, overwriting a
  // newer position another device pushed with a stale local one.
  const Position atLoad{4, 7};

  const Position baseline = SyncedPositionMarker::baselineFor(std::nullopt, atLoad);
  EXPECT_EQ(baseline.spineIndex, 4);
  EXPECT_EQ(baseline.pageNumber, 7);

  EXPECT_FALSE(SyncedPositionMarker::positionNeedsPush(std::nullopt, atLoad, Position{4, 7}));
  EXPECT_TRUE(SyncedPositionMarker::positionNeedsPush(std::nullopt, atLoad, Position{4, 8}));
}

TEST(SyncedPositionBaseline, MarkerEqualToTheCurrentPositionMeansNothingToSend) {
  const std::optional<Position> marker = Position{4, 7};

  EXPECT_FALSE(SyncedPositionMarker::positionNeedsPush(marker, Position{4, 7}, Position{4, 7}));
}

TEST(SyncedPositionBaseline, MarkerBehindTheResumePositionStillNeedsAPush) {
  // The regression this whole change exists for. Last session's upload failed,
  // so the marker holds page 3 while the book resumed at page 5 and the reader
  // never turned a page. Pre-fix the baseline was the resume position and this
  // reported false -- the position was then lost permanently.
  const std::optional<Position> marker = Position{4, 3};

  EXPECT_TRUE(SyncedPositionMarker::positionNeedsPush(marker, Position{4, 5}, Position{4, 5}));
}

TEST(SyncedPositionBaseline, MarkerInAnotherSpineItemStillNeedsAPush) {
  const std::optional<Position> marker = Position{3, 5};

  EXPECT_TRUE(SyncedPositionMarker::positionNeedsPush(marker, Position{4, 5}, Position{4, 5}));
}

}  // namespace
