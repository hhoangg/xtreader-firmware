// GET /wallpapers/manifest's NDJSON body, fed the way HttpDownloader hands
// it out: in arbitrary chunks that can split anywhere, including mid-line and
// mid-UTF-8-sequence.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "WallpaperManifest.h"

namespace {

struct Collected {
  std::vector<std::string> ids;
  std::vector<uint64_t> sizes;
  std::vector<bool> deletedFlags;
  bool sawTrailer = false;
  ManifestTrailer trailer;
};

bool onEntry(void* ctxPtr, const wallpaper_manifest::Entry& entry) {
  auto* c = static_cast<Collected*>(ctxPtr);
  c->ids.push_back(entry.id);
  c->sizes.push_back(entry.sizeBytes);
  c->deletedFlags.push_back(entry.deleted);
  return true;
}

void onTrailer(void* ctxPtr, const ManifestTrailer& trailer) {
  auto* c = static_cast<Collected*>(ctxPtr);
  c->sawTrailer = true;
  c->trailer = trailer;
}

// Feeds `body` in fixed-size pieces, so a boundary lands wherever it lands.
bool feedInChunks(wallpaper_manifest::StreamParser& parser, const std::string& body, const size_t chunk) {
  for (size_t offset = 0; offset < body.size(); offset += chunk) {
    const size_t len = std::min(chunk, body.size() - offset);
    if (!parser.feed(reinterpret_cast<const uint8_t*>(body.data() + offset), len)) return false;
  }
  return true;
}

// A row exactly as apps/api/src/routes/wallpapers.ts's toEntry() serialises it.
const char kRow[] = R"({"id":"wlp_Ab-1_cD","name":"Vịnh Hạ Long","width":480,"height":800,"sizeBytes":96070,)"
                    R"("contentHash":"deadbeef","visibility":"private","updatedAt":1755300000})";

}  // namespace

TEST(WallpaperManifestParser, ReadsARealRow) {
  wallpaper_manifest::Line line;
  ASSERT_TRUE(wallpaper_manifest::parseLine(kRow, sizeof(kRow) - 1, line));
  EXPECT_EQ(line.kind, wallpaper_manifest::Line::Kind::ENTRY);
  EXPECT_EQ(line.entry.id, "wlp_Ab-1_cD");
  EXPECT_EQ(line.entry.sizeBytes, 96070u);
  EXPECT_FALSE(line.entry.deleted);
}

TEST(WallpaperManifestParser, ReadsATrailer) {
  const std::string trailer = R"({"done":true,"nextCursor":"wlp_x","totalCount":7})";
  wallpaper_manifest::Line line;
  ASSERT_TRUE(wallpaper_manifest::parseLine(trailer.data(), trailer.size(), line));
  EXPECT_EQ(line.kind, wallpaper_manifest::Line::Kind::TRAILER);
  EXPECT_TRUE(line.trailer.done);
  EXPECT_TRUE(line.trailer.hasNextCursor);
  EXPECT_EQ(line.trailer.nextCursor, "wlp_x");
  EXPECT_EQ(line.trailer.totalCount, 7u);
}

TEST(WallpaperManifestParser, TreatsANullCursorAsTheLastPage) {
  const std::string trailer = R"({"done":true,"nextCursor":null,"totalCount":2})";
  wallpaper_manifest::Line line;
  ASSERT_TRUE(wallpaper_manifest::parseLine(trailer.data(), trailer.size(), line));
  EXPECT_EQ(line.kind, wallpaper_manifest::Line::Kind::TRAILER);
  EXPECT_FALSE(line.trailer.hasNextCursor);
}

TEST(WallpaperManifestParser, RejectsSomethingThatIsNeitherShape) {
  wallpaper_manifest::Line line;
  EXPECT_FALSE(wallpaper_manifest::parseLine("", 0, line));
  const std::string noId = R"({"name":"x","sizeBytes":1})";
  EXPECT_FALSE(wallpaper_manifest::parseLine(noId.data(), noId.size(), line));
  const std::string notJson = "not json at all";
  EXPECT_FALSE(wallpaper_manifest::parseLine(notJson.data(), notJson.size(), line));
}

TEST(WallpaperManifestParser, SurvivesEveryChunkBoundary) {
  std::string body = kRow;
  body += "\n";
  body += R"({"id":"wlp_second","name":"Đà Lạt","width":480,"height":800,"sizeBytes":96070,)"
          R"("contentHash":"cafe","visibility":"public","updatedAt":1755300001})";
  body += "\n";
  body += R"({"done":true,"nextCursor":null,"totalCount":2})";
  body += "\n";

  // Chunk size 1 splits inside every multi-byte Vietnamese character in the
  // names, which is exactly the case LineChunker exists to make safe.
  for (const size_t chunk : {size_t{1}, size_t{3}, size_t{17}, size_t{1024}}) {
    Collected collected;
    wallpaper_manifest::StreamParser parser(&onEntry, &onTrailer, &collected);
    ASSERT_TRUE(feedInChunks(parser, body, chunk)) << chunk;
    EXPECT_FALSE(parser.hasError()) << chunk;
    EXPECT_EQ(collected.ids, (std::vector<std::string>{"wlp_Ab-1_cD", "wlp_second"})) << chunk;
    EXPECT_EQ(collected.sizes, (std::vector<uint64_t>{96070u, 96070u})) << chunk;
    ASSERT_TRUE(collected.sawTrailer) << chunk;
    EXPECT_TRUE(parser.hasTrailer()) << chunk;
    EXPECT_EQ(parser.trailer().totalCount, 2u) << chunk;
    EXPECT_FALSE(parser.trailer().hasNextCursor) << chunk;
  }
}

TEST(WallpaperManifestParser, CarriesATombstoneThrough) {
  const std::string body = R"({"id":"wlp_gone","name":"x","width":480,"height":800,"sizeBytes":96070,)"
                           R"("contentHash":"c","visibility":"private","updatedAt":1,"deleted":true})"
                           "\n"
                           R"({"done":true,"nextCursor":null,"totalCount":1})"
                           "\n";
  Collected collected;
  wallpaper_manifest::StreamParser parser(&onEntry, &onTrailer, &collected);
  ASSERT_TRUE(feedInChunks(parser, body, 8));
  ASSERT_EQ(collected.deletedFlags.size(), 1u);
  EXPECT_TRUE(collected.deletedFlags[0]);
}

TEST(WallpaperManifestParser, AbortsThePageOnARowThatIsNeitherShape) {
  // A row carrying neither an "id" nor "done" is not something to skip past:
  // it means the response is not the manifest this device asked for, so the
  // page is abandoned rather than half-applied.
  const std::string body = R"({"name":"x","sizeBytes":1})"
                           "\n";
  Collected collected;
  wallpaper_manifest::StreamParser parser(&onEntry, &onTrailer, &collected);
  EXPECT_FALSE(parser.feed(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
  EXPECT_FALSE(parser.hasTrailer());
  EXPECT_TRUE(collected.ids.empty());
}
