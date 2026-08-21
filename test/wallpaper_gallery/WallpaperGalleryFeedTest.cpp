// GET /wallpapers/gallery's NDJSON body, fed the way HttpDownloader hands it
// out: in arbitrary chunks that can split anywhere, including mid-line and
// mid-UTF-8-sequence. Plus the request paths those bodies are fetched from,
// where the interesting case is an opaque cursor that must not be able to grow
// a second query parameter.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "WallpaperGalleryFeed.h"

namespace {

struct Collected {
  std::vector<wallpaper_gallery::Entry> entries;
  bool sawTrailer = false;
  ManifestTrailer trailer;
};

bool onEntry(void* ctxPtr, const wallpaper_gallery::Entry& entry) {
  static_cast<Collected*>(ctxPtr)->entries.push_back(entry);
  return true;
}

void onTrailer(void* ctxPtr, const ManifestTrailer& trailer) {
  auto* c = static_cast<Collected*>(ctxPtr);
  c->sawTrailer = true;
  c->trailer = trailer;
}

// Feeds `body` in fixed-size pieces, so a boundary lands wherever it lands.
bool feedInChunks(wallpaper_gallery::StreamParser& parser, const std::string& body, const size_t chunk) {
  for (size_t offset = 0; offset < body.size(); offset += chunk) {
    const size_t len = std::min(chunk, body.size() - offset);
    if (!parser.feed(reinterpret_cast<const uint8_t*>(body.data() + offset), len)) return false;
  }
  return true;
}

// A gallery row exactly as the contract describes it.
const char kRow[] = R"({"id":"wlp_Ab-1_cD","name":"Vịnh Hạ Long","width":480,"height":800,"sizeBytes":96070,)"
                    R"("hasThumbnail":true,"attachCount":42,"ownerName":"Hoàng","attached":false})";

}  // namespace

TEST(WallpaperGalleryFeed, ReadsARealGalleryRow) {
  wallpaper_gallery::Line line;
  ASSERT_TRUE(wallpaper_gallery::parseLine(kRow, sizeof(kRow) - 1, line));
  EXPECT_EQ(line.kind, wallpaper_gallery::Line::Kind::ENTRY);
  EXPECT_EQ(line.entry.id, "wlp_Ab-1_cD");
  EXPECT_EQ(line.entry.name, "Vịnh Hạ Long");
  EXPECT_EQ(line.entry.ownerName, "Hoàng");
  EXPECT_EQ(line.entry.sizeBytes, 96070u);
  EXPECT_EQ(line.entry.width, 480u);
  EXPECT_EQ(line.entry.height, 800u);
  EXPECT_EQ(line.entry.attachCount, 42u);
  EXPECT_TRUE(line.entry.hasThumbnail);
  EXPECT_FALSE(line.entry.attached);
  EXPECT_FALSE(line.entry.deleted);
}

// The "on this device" tab reads GET /wallpapers/manifest, whose rows carry
// neither ownerName nor attachCount. The missing fields must default rather
// than fail the row.
TEST(WallpaperGalleryFeed, ReadsAManifestRowWithTheGalleryFieldsAbsent) {
  const std::string row = R"({"id":"wlp_x","name":"Ha Long","width":480,"height":800,"sizeBytes":96070,)"
                          R"("contentHash":"deadbeef","visibility":"private","updatedAt":1755300000})";
  wallpaper_gallery::Line line;
  ASSERT_TRUE(wallpaper_gallery::parseLine(row.data(), row.size(), line));
  EXPECT_EQ(line.kind, wallpaper_gallery::Line::Kind::ENTRY);
  EXPECT_EQ(line.entry.id, "wlp_x");
  EXPECT_EQ(line.entry.name, "Ha Long");
  EXPECT_TRUE(line.entry.ownerName.empty());
  EXPECT_EQ(line.entry.attachCount, 0u);
  EXPECT_FALSE(line.entry.hasThumbnail);
}

TEST(WallpaperGalleryFeed, ReadsATrailer) {
  const std::string trailer = R"({"done":true,"nextCursor":"eyJpZCI6MX0","totalCount":180})";
  wallpaper_gallery::Line line;
  ASSERT_TRUE(wallpaper_gallery::parseLine(trailer.data(), trailer.size(), line));
  EXPECT_EQ(line.kind, wallpaper_gallery::Line::Kind::TRAILER);
  EXPECT_TRUE(line.trailer.done);
  EXPECT_TRUE(line.trailer.hasNextCursor);
  EXPECT_EQ(line.trailer.nextCursor, "eyJpZCI6MX0");
  EXPECT_EQ(line.trailer.totalCount, 180u);
}

TEST(WallpaperGalleryFeed, TreatsANullCursorAsTheLastPage) {
  const std::string trailer = R"({"done":true,"nextCursor":null,"totalCount":2})";
  wallpaper_gallery::Line line;
  ASSERT_TRUE(wallpaper_gallery::parseLine(trailer.data(), trailer.size(), line));
  EXPECT_FALSE(line.trailer.hasNextCursor);
}

TEST(WallpaperGalleryFeed, RejectsSomethingThatIsNeitherShape) {
  const std::string junk = R"({"hello":"world"})";
  wallpaper_gallery::Line line;
  EXPECT_FALSE(wallpaper_gallery::parseLine(junk.data(), junk.size(), line));
  EXPECT_EQ(line.kind, wallpaper_gallery::Line::Kind::INVALID);
  EXPECT_FALSE(wallpaper_gallery::parseLine("", 0, line));
}

TEST(WallpaperGalleryFeed, SurvivesAChunkBoundaryAnywhere) {
  std::string body;
  body += kRow;
  body += "\n";
  body += R"({"id":"wlp_second","name":"Đà Lạt sương mù","ownerName":"Mai","attachCount":3,"attached":true})";
  body += "\n";
  body += R"({"done":true,"nextCursor":null,"totalCount":2})";
  body += "\n";

  for (size_t chunk = 1; chunk <= body.size(); chunk++) {
    Collected collected;
    wallpaper_gallery::StreamParser parser(&onEntry, &onTrailer, &collected);
    ASSERT_TRUE(feedInChunks(parser, body, chunk)) << "chunk " << chunk;
    ASSERT_EQ(collected.entries.size(), 2u) << "chunk " << chunk;
    EXPECT_EQ(collected.entries[0].name, "Vịnh Hạ Long") << "chunk " << chunk;
    EXPECT_EQ(collected.entries[1].name, "Đà Lạt sương mù") << "chunk " << chunk;
    EXPECT_TRUE(collected.entries[1].attached) << "chunk " << chunk;
    EXPECT_TRUE(collected.sawTrailer) << "chunk " << chunk;
    EXPECT_TRUE(parser.hasTrailer()) << "chunk " << chunk;
  }
}

// --- Display-string bounds --------------------------------------------------

TEST(WallpaperGalleryFeed, TruncatesOnAUtf8Boundary) {
  // "ạ" is three bytes, so "aạbc" is a, E1, BA, A1, b, c.
  const std::string text = "aạbc";
  ASSERT_EQ(text.size(), 6u);
  // A cut that already lands on a boundary keeps the whole character.
  EXPECT_EQ(wallpaper_gallery::truncateUtf8(text.data(), text.size(), 4), "aạ");
  // A cut inside the sequence walks back off it rather than emitting half a
  // character.
  EXPECT_EQ(wallpaper_gallery::truncateUtf8(text.data(), text.size(), 3), "a");
  EXPECT_EQ(wallpaper_gallery::truncateUtf8(text.data(), text.size(), 2), "a");
  EXPECT_EQ(wallpaper_gallery::truncateUtf8(text.data(), text.size(), 100), text);
  EXPECT_EQ(wallpaper_gallery::truncateUtf8(text.data(), text.size(), 0), "");
}

TEST(WallpaperGalleryFeed, BoundsTheDisplayStringsItKeeps) {
  const std::string longName(200, 'x');
  const std::string longOwner(200, 'y');
  const std::string row = R"({"id":"wlp_x","name":")" + longName + R"(","ownerName":")" + longOwner + R"("})";
  wallpaper_gallery::Line line;
  ASSERT_TRUE(wallpaper_gallery::parseLine(row.data(), row.size(), line));
  EXPECT_EQ(line.entry.name.size(), wallpaper_gallery::MAX_NAME_BYTES);
  EXPECT_EQ(line.entry.ownerName.size(), wallpaper_gallery::MAX_OWNER_BYTES);
}

// --- Request paths ----------------------------------------------------------

TEST(WallpaperGalleryFeed, BuildsTheGalleryPaths) {
  EXPECT_EQ(wallpaper_gallery::requestPath(wallpaper_gallery::Sort::Popular, "", 12),
            "/wallpapers/gallery?sort=popular&limit=12");
  EXPECT_EQ(wallpaper_gallery::requestPath(wallpaper_gallery::Sort::Recent, "", 12),
            "/wallpapers/gallery?sort=recent&limit=12");
  EXPECT_EQ(wallpaper_gallery::requestPath(wallpaper_gallery::Sort::Mine, "", 12), "/wallpapers/manifest?limit=12");
}

TEST(WallpaperGalleryFeed, PercentEncodesAnOpaqueCursor) {
  EXPECT_EQ(wallpaper_gallery::encodeCursor("abcXYZ019-._~"), "abcXYZ019-._~");
  EXPECT_EQ(wallpaper_gallery::encodeCursor("a b"), "a%20b");
  EXPECT_EQ(wallpaper_gallery::encodeCursor("{\"id\":1}"), "%7B%22id%22%3A1%7D");
  // The one that matters: a cursor cannot smuggle in a second parameter.
  EXPECT_EQ(wallpaper_gallery::encodeCursor("x&limit=9999"), "x%26limit%3D9999");
  EXPECT_EQ(wallpaper_gallery::encodeCursor("x/../../admin"), "x%2F..%2F..%2Fadmin");
}

TEST(WallpaperGalleryFeed, RefusesAnAbsurdlyLongCursor) {
  EXPECT_EQ(wallpaper_gallery::encodeCursor(std::string(wallpaper_gallery::MAX_CURSOR_LEN + 1, 'a')), "");
  // ...and the path then simply omits it, ending pagination rather than
  // sending something unchecked.
  EXPECT_EQ(wallpaper_gallery::requestPath(wallpaper_gallery::Sort::Popular,
                                           std::string(wallpaper_gallery::MAX_CURSOR_LEN + 1, 'a'), 6),
            "/wallpapers/gallery?sort=popular&limit=6");
}

TEST(WallpaperGalleryFeed, AppendsAnEncodedCursor) {
  EXPECT_EQ(wallpaper_gallery::requestPath(wallpaper_gallery::Sort::Popular, "eyJpZCI6MX0=", 6),
            "/wallpapers/gallery?sort=popular&limit=6&cursor=eyJpZCI6MX0%3D");
}

// The manifest cursor is a wallpaper id, so it gets the id alphabet check
// wallpaper sync already applies -- not percent-encoding.
TEST(WallpaperGalleryFeed, ChecksTheManifestCursorAgainstTheIdAlphabet) {
  EXPECT_EQ(wallpaper_gallery::requestPath(wallpaper_gallery::Sort::Mine, "wlp_abc", 6),
            "/wallpapers/manifest?limit=6&cursor=wlp_abc");
  EXPECT_EQ(wallpaper_gallery::requestPath(wallpaper_gallery::Sort::Mine, "wlp/../x", 6),
            "/wallpapers/manifest?limit=6");
}
