#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "LineChunker.h"
#include "ManifestLineParser.h"
#include "ManifestPager.h"
#include "ManifestStreamParser.h"

namespace {

// Straight from crosspoint-sync docs/API.md's GET /library/manifest example.
const std::string kEntryLine1 =
    R"({"id":"bok_1","path":"/Kỹ năng/Đắc Nhân Tâm.epub","sizeBytes":3014656,"contentHash":"aaa111","updatedAt":1755300000})";
const std::string kEntryLine2 =
    R"({"id":"bok_2","path":"/Văn học/Nhà Giả Kim.epub","sizeBytes":1441792,"contentHash":"bbb222","updatedAt":1755300001})";
const std::string kTrailerLine = R"({"done":true,"nextCursor":"/Văn học/Nhà Giả Kim.epub","totalCount":842})";
const std::string kTrailerLineDone = R"({"done":true,"nextCursor":null,"totalCount":842})";

bool collectLine(void* ctx, const char* line, size_t len) {
  static_cast<std::vector<std::string>*>(ctx)->emplace_back(line, len);
  return true;
}

std::vector<std::string> collectLines(const std::string& body, size_t chunkSize) {
  std::vector<std::string> lines;
  LineChunker chunker(&collectLine, &lines, 2048);
  for (size_t i = 0; i < body.size(); i += chunkSize) {
    const size_t n = std::min(chunkSize, body.size() - i);
    EXPECT_TRUE(chunker.feed(reinterpret_cast<const uint8_t*>(body.data() + i), n));
  }
  return lines;
}

// --- LineChunker -------------------------------------------------------

TEST(LineChunker, SplitsWholeBufferIntoLines) {
  const std::string body = kEntryLine1 + "\n" + kEntryLine2 + "\n" + kTrailerLine + "\n";
  const auto lines = collectLines(body, body.size());  // one feed() call, whole body
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], kEntryLine1);
  EXPECT_EQ(lines[1], kEntryLine2);
  EXPECT_EQ(lines[2], kTrailerLine);
}

TEST(LineChunker, ReassemblesALineSplitAcrossAChunkBoundary) {
  const std::string body = kEntryLine1 + "\n" + kEntryLine2 + "\n";
  // Feed one byte at a time -- the most aggressive possible mid-line split,
  // including splitting kEntryLine1's own multi-byte UTF-8 bytes one at a time.
  const auto lines = collectLines(body, 1);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], kEntryLine1);
  EXPECT_EQ(lines[1], kEntryLine2);
}

TEST(LineChunker, ReassemblesALineSplitMidUtf8Sequence) {
  // "ỹ" (U+1EF9, in "Kỹ năng") encodes as the 3 bytes E1 BB B9. Split the
  // chunk boundary to land one byte into that sequence -- exactly the case
  // the class comment describes: a chunk boundary that falls mid-line *and*
  // mid-UTF-8-sequence must not corrupt or misinterpret the character.
  const std::string body = kEntryLine1 + "\n";
  const size_t utf8Start = body.find("\xE1\xBB\xB9");
  ASSERT_NE(utf8Start, std::string::npos);
  const size_t splitAt = utf8Start + 1;  // mid-sequence: after the lead byte only

  std::vector<std::string> lines;
  LineChunker chunker(&collectLine, &lines, 2048);
  ASSERT_TRUE(chunker.feed(reinterpret_cast<const uint8_t*>(body.data()), splitAt));
  ASSERT_TRUE(chunker.feed(reinterpret_cast<const uint8_t*>(body.data() + splitAt), body.size() - splitAt));

  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], kEntryLine1);
}

bool acceptAnyLine(void*, const char*, size_t) { return true; }

TEST(LineChunker, OversizedLineIsAnError) {
  const std::string hugeLine(100, 'x');  // capacity below is smaller than this
  LineChunker chunker(&acceptAnyLine, nullptr, 32);
  const std::string body = hugeLine + "\n";
  EXPECT_FALSE(chunker.feed(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
  EXPECT_TRUE(chunker.hasError());
  EXPECT_TRUE(chunker.overflowed());
}

// --- ManifestLineParser --------------------------------------------------

TEST(ManifestLineParser, ParsesAnEntryLine) {
  ManifestLine out;
  ASSERT_TRUE(parseManifestLine(kEntryLine1.data(), kEntryLine1.size(), out));
  ASSERT_EQ(out.kind, ManifestLine::Kind::ENTRY);
  EXPECT_EQ(out.entry.id, "bok_1");
  EXPECT_EQ(out.entry.path, "/Kỹ năng/Đắc Nhân Tâm.epub");
  EXPECT_EQ(out.entry.sizeBytes, 3014656u);
  EXPECT_EQ(out.entry.contentHash, "aaa111");
  EXPECT_EQ(out.entry.updatedAt, 1755300000u);
  EXPECT_FALSE(out.entry.deleted);
}

TEST(ManifestLineParser, ParsesATrailerWithNextCursor) {
  ManifestLine out;
  ASSERT_TRUE(parseManifestLine(kTrailerLine.data(), kTrailerLine.size(), out));
  ASSERT_EQ(out.kind, ManifestLine::Kind::TRAILER);
  EXPECT_TRUE(out.trailer.done);
  EXPECT_TRUE(out.trailer.hasNextCursor);
  EXPECT_EQ(out.trailer.nextCursor, "/Văn học/Nhà Giả Kim.epub");
  EXPECT_EQ(out.trailer.totalCount, 842u);
}

TEST(ManifestLineParser, RecognisesTheTrailerWithNullNextCursorAsTheLastPage) {
  ManifestLine out;
  ASSERT_TRUE(parseManifestLine(kTrailerLineDone.data(), kTrailerLineDone.size(), out));
  ASSERT_EQ(out.kind, ManifestLine::Kind::TRAILER);
  EXPECT_TRUE(out.trailer.done);
  EXPECT_FALSE(out.trailer.hasNextCursor);
  EXPECT_EQ(out.trailer.totalCount, 842u);
}

TEST(ManifestLineParser, ParsesATombstoneEntry) {
  const std::string line = R"({"id":"bok_3","path":"/Old/Book.epub","deleted":true,"updatedAt":1755300002})";
  ManifestLine out;
  ASSERT_TRUE(parseManifestLine(line.data(), line.size(), out));
  ASSERT_EQ(out.kind, ManifestLine::Kind::ENTRY);
  EXPECT_EQ(out.entry.id, "bok_3");
  EXPECT_TRUE(out.entry.deleted);
}

TEST(ManifestLineParser, RejectsSyntacticallyInvalidJson) {
  const std::string line = R"({"id":"bok_1","path":)";  // truncated / unbalanced
  ManifestLine out;
  EXPECT_FALSE(parseManifestLine(line.data(), line.size(), out));
  EXPECT_EQ(out.kind, ManifestLine::Kind::INVALID);
}

TEST(ManifestLineParser, RejectsValidJsonThatIsNeitherAnEntryNorATrailer) {
  const std::string line = R"({"foo":"bar"})";  // valid JSON, but no id/path and no done
  ManifestLine out;
  EXPECT_FALSE(parseManifestLine(line.data(), line.size(), out));
  EXPECT_EQ(out.kind, ManifestLine::Kind::INVALID);
}

TEST(ManifestLineParser, RejectsAnEmptyLine) {
  ManifestLine out;
  EXPECT_FALSE(parseManifestLine("", 0, out));
}

TEST(ManifestLineParser, RejectsAnEntryWhosePathExceedsTheJsonTokenBuffer) {
  // StreamingJsonParser::TOKEN_BUF_SIZE (lib/JsonParser/StreamingJsonParser.h)
  // is 512 bytes; a string value longer than that overflows the token buffer
  // and the parser skips calling onString for it entirely (see
  // StreamingJsonParser::emitToken's `!tokenOverflow` guard) rather than
  // handing back a truncated value. That means "path" is never assigned, so
  // sawPath never becomes true, and this line comes back as INVALID rather
  // than as an ENTRY with a wrong/truncated path -- the loss is already a
  // hard parse failure, not a silent truncation into a wrong-but-valid path.
  const std::string oversizedPath = "/" + std::string(600, 'a') + ".epub";
  const std::string line = R"({"id":"bok_1","path":")" + oversizedPath + R"("})";
  ManifestLine out;
  EXPECT_FALSE(parseManifestLine(line.data(), line.size(), out));
  EXPECT_EQ(out.kind, ManifestLine::Kind::INVALID);
}

// --- ManifestStreamParser -------------------------------------------------

struct CollectedPage {
  std::vector<ManifestEntry> entries;
  bool sawTrailer = false;
  ManifestTrailer trailer;
};

bool onCollectedEntry(void* ctx, const ManifestEntry& e) {
  static_cast<CollectedPage*>(ctx)->entries.push_back(e);
  return true;
}

void onCollectedTrailer(void* ctx, const ManifestTrailer& t) {
  auto* page = static_cast<CollectedPage*>(ctx);
  page->sawTrailer = true;
  page->trailer = t;
}

bool feedPage(CollectedPage& page, const std::string& body, size_t chunkSize) {
  ManifestStreamParser parser(&onCollectedEntry, &onCollectedTrailer, &page);
  bool ok = true;
  for (size_t i = 0; i < body.size() && ok; i += chunkSize) {
    const size_t n = std::min(chunkSize, body.size() - i);
    ok = parser.feed(reinterpret_cast<const uint8_t*>(body.data() + i), n);
  }
  return ok;
}

TEST(ManifestStreamParser, ParsesAWholePageInOneChunk) {
  const std::string body = kEntryLine1 + "\n" + kEntryLine2 + "\n" + kTrailerLine + "\n";
  CollectedPage page;
  EXPECT_TRUE(feedPage(page, body, body.size()));
  ASSERT_EQ(page.entries.size(), 2u);
  EXPECT_EQ(page.entries[0].id, "bok_1");
  EXPECT_EQ(page.entries[1].id, "bok_2");
  ASSERT_TRUE(page.sawTrailer);
  EXPECT_EQ(page.trailer.nextCursor, "/Văn học/Nhà Giả Kim.epub");
}

TEST(ManifestStreamParser, ParsesTheSamePageIdenticallyWhenChunkedOneByteAtATime) {
  const std::string body = kEntryLine1 + "\n" + kEntryLine2 + "\n" + kTrailerLine + "\n";
  CollectedPage wholeBody;
  CollectedPage byteAtATime;
  ASSERT_TRUE(feedPage(wholeBody, body, body.size()));
  ASSERT_TRUE(feedPage(byteAtATime, body, 1));

  ASSERT_EQ(wholeBody.entries.size(), byteAtATime.entries.size());
  for (size_t i = 0; i < wholeBody.entries.size(); ++i) {
    EXPECT_EQ(wholeBody.entries[i].id, byteAtATime.entries[i].id);
    EXPECT_EQ(wholeBody.entries[i].path, byteAtATime.entries[i].path);
  }
  EXPECT_EQ(wholeBody.trailer.nextCursor, byteAtATime.trailer.nextCursor);
}

TEST(ManifestStreamParser, AbortsThePageOnAMalformedLine) {
  const std::string body = kEntryLine1 + "\nnot json at all\n" + kTrailerLine + "\n";
  CollectedPage page;
  EXPECT_FALSE(feedPage(page, body, body.size()));
  // The first (valid) entry was still delivered before the malformed one
  // was hit -- an abort loses nothing already-persisted by the caller.
  ASSERT_EQ(page.entries.size(), 1u);
  EXPECT_FALSE(page.sawTrailer);
}

// --- ManifestPager ---------------------------------------------------------

TEST(ManifestPager, FollowsACursorAcrossTwoPagesThenStops) {
  ManifestPager pager;

  ManifestTrailer page1Trailer;
  page1Trailer.done = true;
  page1Trailer.hasNextCursor = true;
  page1Trailer.nextCursor = "/Văn học/Nhà Giả Kim.epub";
  page1Trailer.totalCount = 842;
  EXPECT_TRUE(pager.onPageTrailer(page1Trailer));
  EXPECT_EQ(pager.pagesFetched(), 1u);
  EXPECT_EQ(pager.nextCursor(), "/Văn học/Nhà Giả Kim.epub");
  EXPECT_FALSE(pager.exceededPageLimit());

  ManifestTrailer page2Trailer;
  page2Trailer.done = true;
  page2Trailer.hasNextCursor = false;  // null -- last page
  page2Trailer.totalCount = 842;
  EXPECT_FALSE(pager.onPageTrailer(page2Trailer));
  EXPECT_EQ(pager.pagesFetched(), 2u);
  EXPECT_TRUE(pager.nextCursor().empty());
  EXPECT_FALSE(pager.exceededPageLimit());
}

TEST(ManifestPager, BoundsTheNumberOfPagesAgainstARunawayServer) {
  ManifestPager pager(/*maxPages=*/2);

  ManifestTrailer trailerWithMore;
  trailerWithMore.done = true;
  trailerWithMore.hasNextCursor = true;
  trailerWithMore.nextCursor = "/next";

  EXPECT_TRUE(pager.onPageTrailer(trailerWithMore));   // page 1 of 2
  EXPECT_FALSE(pager.onPageTrailer(trailerWithMore));  // page 2 hits the bound, even though more was offered
  EXPECT_TRUE(pager.exceededPageLimit());
  EXPECT_TRUE(pager.nextCursor().empty());
}

}  // namespace
