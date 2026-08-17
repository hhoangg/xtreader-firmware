#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "ManifestIndexFormat.h"
#include "ManifestIndexMerge.h"
#include "ManifestIndexQuery.h"

namespace {

ManifestIndexRecord makeRecord(const std::string& id, const std::string& path, uint64_t sizeBytes = 100,
                               const std::string& hash = "hash", uint64_t updatedAt = 1755300000,
                               bool downloaded = false) {
  ManifestIndexRecord r;
  r.id = id;
  r.path = path;
  r.sizeBytes = sizeBytes;
  r.contentHash = hash;
  r.updatedAt = updatedAt;
  r.downloaded = downloaded;
  return r;
}

// --- ManifestIndexFormat: format/parse round trip -------------------------

TEST(ManifestIndexFormat, RoundTripsAllFields) {
  const auto record = makeRecord("bok_1", "/Kỹ năng/Đắc Nhân Tâm.epub", 3014656, "aaa111deadbeef", 1755300000, true);
  const std::string line = formatIndexLine(record);
  EXPECT_EQ(line.back(), '\n');

  ManifestIndexRecord parsed;
  // formatIndexLine includes the trailing '\n'; parseIndexLine's contract
  // (like parseManifestLine's) is a line with that already stripped -- as
  // LineChunker would hand it over.
  ASSERT_TRUE(parseIndexLine(line.data(), line.size() - 1, parsed));
  EXPECT_EQ(parsed.id, record.id);
  EXPECT_EQ(parsed.path, record.path);
  EXPECT_EQ(parsed.sizeBytes, record.sizeBytes);
  EXPECT_EQ(parsed.contentHash, record.contentHash);
  EXPECT_EQ(parsed.updatedAt, record.updatedAt);
  EXPECT_EQ(parsed.downloaded, record.downloaded);
}

TEST(ManifestIndexFormat, RejectsALineWithTooFewFields) {
  const std::string malformed = "bok_1|/path.epub|100|hash";  // missing updatedAt + downloaded
  ManifestIndexRecord out;
  EXPECT_FALSE(parseIndexLine(malformed.data(), malformed.size(), out));
}

TEST(ManifestIndexFormat, RejectsALineWithTooManyFields) {
  const std::string malformed = "bok_1|/path.epub|100|hash|1755300000|0|extra";
  ManifestIndexRecord out;
  EXPECT_FALSE(parseIndexLine(malformed.data(), malformed.size(), out));
}

TEST(ManifestIndexFormat, RejectsNonNumericSizeBytes) {
  const std::string malformed = "bok_1|/path.epub|not-a-number|hash|1755300000|0";
  ManifestIndexRecord out;
  EXPECT_FALSE(parseIndexLine(malformed.data(), malformed.size(), out));
}

TEST(ManifestIndexFormat, RejectsABadDownloadedFlag) {
  const std::string malformed = "bok_1|/path.epub|100|hash|1755300000|maybe";
  ManifestIndexRecord out;
  EXPECT_FALSE(parseIndexLine(malformed.data(), malformed.size(), out));
}

TEST(ManifestIndexFormat, RejectsAnEmptyLine) {
  ManifestIndexRecord out;
  EXPECT_FALSE(parseIndexLine("", 0, out));
}

// --- ManifestIndexPrefixScan -----------------------------------------------

bool collectMatchedId(void* ctx, const ManifestIndexRecord& r) {
  static_cast<std::vector<std::string>*>(ctx)->push_back(r.id);
  return true;
}

// A small sorted-by-path index spanning three folders, mirroring what
// SyncManifest.cpp would have written from a real manifest sync.
std::string buildSampleIndex() {
  std::string out;
  out += formatIndexLine(makeRecord("bok_1", "/Kỹ năng/Đắc Nhân Tâm.epub"));
  out += formatIndexLine(makeRecord("bok_2", "/Kỹ năng/Nghĩ Giàu Làm Giàu.epub"));
  out += formatIndexLine(makeRecord("bok_3", "/Tiểu thuyết/Số Đỏ.epub"));
  out += formatIndexLine(makeRecord("bok_4", "/Văn học/Chí Phèo.epub"));
  out += formatIndexLine(makeRecord("bok_5", "/Văn học/Nhà Giả Kim.epub", 100, "hash", 1755300000, true));
  return out;
}

TEST(ManifestIndexPrefixScan, ReturnsExactlyTheEntriesUnderAFolder) {
  const std::string index = buildSampleIndex();
  std::vector<std::string> matchedIds;
  ManifestIndexPrefixScan scan("/Kỹ năng/", &collectMatchedId, &matchedIds);
  scan.feed(reinterpret_cast<const uint8_t*>(index.data()), index.size());
  EXPECT_FALSE(scan.hasError());
  EXPECT_EQ(matchedIds, (std::vector<std::string>{"bok_1", "bok_2"}));
}

TEST(ManifestIndexPrefixScan, ReturnsExactlyTheEntriesUnderTheLastFolder) {
  // Regression case for the "runs to EOF" branch: the matching folder is the
  // last one in the index, so the scan never gets a chance to see a record
  // that has moved past the prefix's range -- it must still report both
  // matches and end without an error.
  const std::string index = buildSampleIndex();
  std::vector<std::string> matchedIds;
  ManifestIndexPrefixScan scan("/Văn học/", &collectMatchedId, &matchedIds);
  scan.feed(reinterpret_cast<const uint8_t*>(index.data()), index.size());
  EXPECT_FALSE(scan.hasError());
  EXPECT_EQ(matchedIds, (std::vector<std::string>{"bok_4", "bok_5"}));
}

TEST(ManifestIndexPrefixScan, ReturnsNothingForAFolderThatDoesNotExist) {
  const std::string index = buildSampleIndex();
  std::vector<std::string> matchedIds;
  ManifestIndexPrefixScan scan("/Nonexistent/", &collectMatchedId, &matchedIds);
  scan.feed(reinterpret_cast<const uint8_t*>(index.data()), index.size());
  EXPECT_FALSE(scan.hasError());
  EXPECT_TRUE(matchedIds.empty());
}

TEST(ManifestIndexPrefixScan, WorksWhenFedInSmallChunksAcrossLineBoundaries) {
  const std::string index = buildSampleIndex();
  std::vector<std::string> matchedIds;
  ManifestIndexPrefixScan scan("/Kỹ năng/", &collectMatchedId, &matchedIds);
  for (size_t i = 0; i < index.size(); ++i) {
    scan.feed(reinterpret_cast<const uint8_t*>(index.data() + i), 1);
  }
  EXPECT_FALSE(scan.hasError());
  EXPECT_EQ(matchedIds, (std::vector<std::string>{"bok_1", "bok_2"}));
}

// --- ManifestIndexIdLookup --------------------------------------------------

TEST(ManifestIndexIdLookup, FindsAnEntryByStableId) {
  const std::string index = buildSampleIndex();
  ManifestIndexIdLookup lookup("bok_4");
  lookup.feed(reinterpret_cast<const uint8_t*>(index.data()), index.size());
  EXPECT_FALSE(lookup.hasError());
  ASSERT_TRUE(lookup.found());
  EXPECT_EQ(lookup.record().path, "/Văn học/Chí Phèo.epub");
}

TEST(ManifestIndexIdLookup, TellsWhetherTheBookIsAlreadyDownloaded) {
  const std::string index = buildSampleIndex();
  ManifestIndexIdLookup lookup("bok_5");
  lookup.feed(reinterpret_cast<const uint8_t*>(index.data()), index.size());
  ASSERT_TRUE(lookup.found());
  EXPECT_TRUE(lookup.record().downloaded);

  ManifestIndexIdLookup notDownloaded("bok_1");
  notDownloaded.feed(reinterpret_cast<const uint8_t*>(index.data()), index.size());
  ASSERT_TRUE(notDownloaded.found());
  EXPECT_FALSE(notDownloaded.record().downloaded);
}

TEST(ManifestIndexIdLookup, ReportsNotFoundWithoutErrorForAnUnknownId) {
  const std::string index = buildSampleIndex();
  ManifestIndexIdLookup lookup("bok_does_not_exist");
  lookup.feed(reinterpret_cast<const uint8_t*>(index.data()), index.size());
  EXPECT_FALSE(lookup.hasError());
  EXPECT_FALSE(lookup.found());
}

// --- ManifestIndexDownloadedFlagLocator -------------------------------------

TEST(ManifestIndexDownloadedFlagLocator, FindsTheFlagByteOfAMatchingRecord) {
  const std::string index = buildSampleIndex();
  ManifestIndexDownloadedFlagLocator locator("bok_4");
  locator.feed(reinterpret_cast<const uint8_t*>(index.data()), index.size());
  ASSERT_FALSE(locator.hasError());
  ASSERT_TRUE(locator.found());

  // The byte at flagOffset() must be the '0'/'1' flag character, immediately
  // followed by '\n' -- exactly what markDownloaded() seeks to and overwrites.
  ASSERT_LT(locator.flagOffset() + 1, index.size());
  EXPECT_EQ(index[locator.flagOffset()], '0');
  EXPECT_EQ(index[locator.flagOffset() + 1], '\n');
}

TEST(ManifestIndexDownloadedFlagLocator, FindsTheFlagByteOfTheFirstRecord) {
  // Regression case for offset-tracking starting at 0 rather than some
  // implicit prior line's width.
  const std::string index = buildSampleIndex();
  ManifestIndexDownloadedFlagLocator locator("bok_1");
  locator.feed(reinterpret_cast<const uint8_t*>(index.data()), index.size());
  ASSERT_TRUE(locator.found());
  EXPECT_EQ(index[locator.flagOffset()], '0');
  EXPECT_EQ(index[locator.flagOffset() + 1], '\n');
}

TEST(ManifestIndexDownloadedFlagLocator, FindsTheFlagByteOfAnAlreadyDownloadedRecord) {
  const std::string index = buildSampleIndex();  // bok_5 is written with downloaded=true
  ManifestIndexDownloadedFlagLocator locator("bok_5");
  locator.feed(reinterpret_cast<const uint8_t*>(index.data()), index.size());
  ASSERT_TRUE(locator.found());
  EXPECT_EQ(index[locator.flagOffset()], '1');
}

TEST(ManifestIndexDownloadedFlagLocator, ReportsNotFoundWithoutErrorForAnUnknownId) {
  const std::string index = buildSampleIndex();
  ManifestIndexDownloadedFlagLocator locator("bok_does_not_exist");
  locator.feed(reinterpret_cast<const uint8_t*>(index.data()), index.size());
  EXPECT_FALSE(locator.hasError());
  EXPECT_FALSE(locator.found());
}

TEST(ManifestIndexDownloadedFlagLocator, WorksWhenFedInSmallChunksAcrossLineBoundaries) {
  const std::string index = buildSampleIndex();
  ManifestIndexDownloadedFlagLocator locator("bok_4");
  for (size_t i = 0; i < index.size(); ++i) {
    locator.feed(reinterpret_cast<const uint8_t*>(index.data() + i), 1);
  }
  ASSERT_TRUE(locator.found());
  EXPECT_EQ(index[locator.flagOffset()], '0');
  EXPECT_EQ(index[locator.flagOffset() + 1], '\n');
}

TEST(ManifestIndexDownloadedFlagLocator, OverflowsOnAnUnterminatedLineLongerThanCapacity) {
  ManifestIndexDownloadedFlagLocator locator("bok_1", /*capacity=*/8);
  const std::string noNewline(64, 'x');
  locator.feed(reinterpret_cast<const uint8_t*>(noNewline.data()), noNewline.size());
  EXPECT_TRUE(locator.hasError());
  EXPECT_FALSE(locator.found());
}

// --- ManifestIndexFormat: header round trip ---------------------------------

TEST(ManifestIndexHeader, RoundTrips) {
  const std::string header = formatIndexHeader(1, 1755300000);
  EXPECT_EQ(header.size(), INDEX_HEADER_LEN);
  EXPECT_EQ(header.back(), '\n');

  uint32_t version = 0;
  uint64_t watermark = 0;
  ASSERT_TRUE(parseIndexHeader(header.data(), header.size(), version, watermark));
  EXPECT_EQ(version, 1u);
  EXPECT_EQ(watermark, 1755300000u);
}

TEST(ManifestIndexHeader, RoundTripsZeroValues) {
  const std::string header = formatIndexHeader(0, 0);
  uint32_t version = 99;
  uint64_t watermark = 99;
  ASSERT_TRUE(parseIndexHeader(header.data(), header.size(), version, watermark));
  EXPECT_EQ(version, 0u);
  EXPECT_EQ(watermark, 0u);
}

TEST(ManifestIndexHeader, RoundTripsMaxWatermark) {
  const std::string header = formatIndexHeader(1, UINT64_MAX);
  uint32_t version = 0;
  uint64_t watermark = 0;
  ASSERT_TRUE(parseIndexHeader(header.data(), header.size(), version, watermark));
  EXPECT_EQ(watermark, UINT64_MAX);
}

TEST(ManifestIndexHeader, RejectsWrongLength) {
  uint32_t version = 0;
  uint64_t watermark = 0;
  EXPECT_FALSE(parseIndexHeader("CPIDX", 5, version, watermark));
}

TEST(ManifestIndexHeader, RejectsBadMagic) {
  std::string header = formatIndexHeader(1, 42);
  header[0] = 'X';
  uint32_t version = 0;
  uint64_t watermark = 0;
  EXPECT_FALSE(parseIndexHeader(header.data(), header.size(), version, watermark));
}

TEST(ManifestIndexHeader, RejectsAMissingDelimiter) {
  std::string header = formatIndexHeader(1, 42);
  header[5] = 'x';  // clobber the '|' right after the magic
  uint32_t version = 0;
  uint64_t watermark = 0;
  EXPECT_FALSE(parseIndexHeader(header.data(), header.size(), version, watermark));
}

TEST(ManifestIndexHeader, RejectsANonDigitInVersion) {
  std::string header = formatIndexHeader(1, 42);
  header[6] = 'x';
  uint32_t version = 0;
  uint64_t watermark = 0;
  EXPECT_FALSE(parseIndexHeader(header.data(), header.size(), version, watermark));
}

TEST(ManifestIndexHeader, RejectsANonDigitInWatermark) {
  std::string header = formatIndexHeader(1, 42);
  header[20] = 'x';
  uint32_t version = 0;
  uint64_t watermark = 0;
  EXPECT_FALSE(parseIndexHeader(header.data(), header.size(), version, watermark));
}

TEST(ManifestIndexHeader, AnOldFormatIndexsFirstLineFailsToParseAsAHeader) {
  // A pre-header index's first line is a plain record, e.g. "bok_1|/path.epub|...|0" -- this must
  // not be mistaken for a valid header. This is how an old-firmware index (written before this
  // header existed at all) naturally falls back to a full resync, with no explicit version tag ever
  // having existed in that old format -- see ManifestIndexFormat.h's header comment.
  const std::string oldFirstLine = formatIndexLine(makeRecord("bok_1", "/path.epub"));
  ASSERT_GE(oldFirstLine.size(), INDEX_HEADER_LEN);
  uint32_t version = 0;
  uint64_t watermark = 0;
  EXPECT_FALSE(parseIndexHeader(oldFirstLine.data(), INDEX_HEADER_LEN, version, watermark));
}

// --- ManifestIndexMerge -------------------------------------------------------

bool collectMergedRecords(void* ctx, const ManifestIndexRecord& r) {
  auto* out = static_cast<std::vector<ManifestIndexRecord>*>(ctx);
  out->push_back(r);
  return true;
}

std::vector<std::string> idsOf(const std::vector<ManifestIndexRecord>& records) {
  std::vector<std::string> ids;
  ids.reserve(records.size());
  for (const auto& r : records) ids.push_back(r.id);
  return ids;
}

// A handful of records over ASCII-sortable paths, distinct from buildSampleIndex()'s Vietnamese one:
// the merge-ordering tests below care about exactly how paths compare against each other, which is
// easier to read and gets no help from (and no risk from) UTF-8 byte-order subtleties that are
// already covered elsewhere (ManifestIndexPrefixScan's tests, formatIndexLine's round trip).
std::string buildAsciiIndex() {
  std::string out;
  out += formatIndexLine(makeRecord("id_a", "/a/1.epub"));
  out += formatIndexLine(makeRecord("id_b", "/a/2.epub"));
  out += formatIndexLine(makeRecord("id_c", "/b/1.epub"));
  out += formatIndexLine(makeRecord("id_d", "/c/1.epub"));
  out += formatIndexLine(makeRecord("id_e", "/c/2.epub"));
  return out;
}

TEST(ManifestIndexMerge, RemovesTheFirstRecord) {
  const std::string index = buildSampleIndex();
  std::vector<ManifestIndexRecord> out;
  ManifestIndexMerge merge({"bok_1"}, {}, &collectMergedRecords, &out);
  ASSERT_TRUE(merge.feed(reinterpret_cast<const uint8_t*>(index.data()), index.size()));
  ASSERT_TRUE(merge.finish());
  EXPECT_FALSE(merge.hasError());
  EXPECT_EQ(idsOf(out), (std::vector<std::string>{"bok_2", "bok_3", "bok_4", "bok_5"}));
}

TEST(ManifestIndexMerge, RemovesAMiddleRecord) {
  const std::string index = buildSampleIndex();
  std::vector<ManifestIndexRecord> out;
  ManifestIndexMerge merge({"bok_3"}, {}, &collectMergedRecords, &out);
  ASSERT_TRUE(merge.feed(reinterpret_cast<const uint8_t*>(index.data()), index.size()));
  ASSERT_TRUE(merge.finish());
  EXPECT_FALSE(merge.hasError());
  EXPECT_EQ(idsOf(out), (std::vector<std::string>{"bok_1", "bok_2", "bok_4", "bok_5"}));
}

TEST(ManifestIndexMerge, RemovesTheLastRecord) {
  const std::string index = buildSampleIndex();
  std::vector<ManifestIndexRecord> out;
  ManifestIndexMerge merge({"bok_5"}, {}, &collectMergedRecords, &out);
  ASSERT_TRUE(merge.feed(reinterpret_cast<const uint8_t*>(index.data()), index.size()));
  ASSERT_TRUE(merge.finish());
  EXPECT_FALSE(merge.hasError());
  EXPECT_EQ(idsOf(out), (std::vector<std::string>{"bok_1", "bok_2", "bok_3", "bok_4"}));
}

TEST(ManifestIndexMerge, UntouchedRecordsSurviveByteForByte) {
  // Regression guard for the removal primitive not silently mutating anything it passes through --
  // in particular bok_5's downloaded=true, the one field a plain remove could plausibly clobber.
  const std::string index = buildSampleIndex();
  std::vector<ManifestIndexRecord> out;
  ManifestIndexMerge merge({"bok_1"}, {}, &collectMergedRecords, &out);
  merge.feed(reinterpret_cast<const uint8_t*>(index.data()), index.size());
  merge.finish();
  ASSERT_EQ(out.size(), 4u);
  EXPECT_TRUE(out.back().downloaded);
  EXPECT_EQ(out.back().id, "bok_5");
}

TEST(ManifestIndexMerge, RemovingAnAbsentIdIsANoOp) {
  const std::string index = buildSampleIndex();
  std::vector<ManifestIndexRecord> out;
  ManifestIndexMerge merge({"bok_does_not_exist"}, {}, &collectMergedRecords, &out);
  ASSERT_TRUE(merge.feed(reinterpret_cast<const uint8_t*>(index.data()), index.size()));
  ASSERT_TRUE(merge.finish());
  EXPECT_FALSE(merge.hasError());
  EXPECT_EQ(idsOf(out), (std::vector<std::string>{"bok_1", "bok_2", "bok_3", "bok_4", "bok_5"}));
}

TEST(ManifestIndexMerge, EmptyResultWhenEverythingIsRemoved) {
  std::string index;
  index += formatIndexLine(makeRecord("bok_1", "/a.epub"));
  index += formatIndexLine(makeRecord("bok_2", "/b.epub"));
  std::vector<ManifestIndexRecord> out;
  ManifestIndexMerge merge({"bok_1", "bok_2"}, {}, &collectMergedRecords, &out);
  ASSERT_TRUE(merge.feed(reinterpret_cast<const uint8_t*>(index.data()), index.size()));
  ASSERT_TRUE(merge.finish());
  EXPECT_FALSE(merge.hasError());
  EXPECT_TRUE(out.empty());
}

TEST(ManifestIndexMerge, FinishWithoutFeedingFlushesEveryUpsertForABrandNewIndex) {
  // The from-scratch case: no old index was ever fed (an empty/missing one), so finish() alone must
  // emit the whole delta, still in sorted order.
  std::vector<ManifestIndexRecord> upserts{makeRecord("id_b", "/b.epub"), makeRecord("id_a", "/a.epub")};
  std::sort(upserts.begin(), upserts.end(), [](const auto& a, const auto& b) { return a.path < b.path; });

  std::vector<ManifestIndexRecord> out;
  ManifestIndexMerge merge({}, upserts, &collectMergedRecords, &out);
  EXPECT_TRUE(merge.finish());
  EXPECT_FALSE(merge.hasError());
  EXPECT_EQ(idsOf(out), (std::vector<std::string>{"id_a", "id_b"}));
}

TEST(ManifestIndexMerge, AppliesAnUpdateADeleteAndAnInsertInOnePass) {
  const std::string index = buildAsciiIndex();

  // id_b updated in place (same path, new contents); id_d tombstoned; a brand new id_f inserted
  // between id_c and id_d's old positions.
  std::vector<ManifestIndexRecord> upserts{makeRecord("id_b", "/a/2.epub", 999, "hash-updated"),
                                           makeRecord("id_f", "/b/2.epub")};
  std::sort(upserts.begin(), upserts.end(), [](const auto& a, const auto& b) { return a.path < b.path; });

  std::vector<ManifestIndexRecord> out;
  ManifestIndexMerge merge({"id_b", "id_d"}, upserts, &collectMergedRecords, &out);
  ASSERT_TRUE(merge.feed(reinterpret_cast<const uint8_t*>(index.data()), index.size()));
  ASSERT_TRUE(merge.finish());
  EXPECT_FALSE(merge.hasError());

  EXPECT_EQ(idsOf(out), (std::vector<std::string>{"id_a", "id_b", "id_c", "id_f", "id_e"}));
  // The updated id_b carries the delta's contents, not the old index's.
  const auto updated = std::find_if(out.begin(), out.end(), [](const auto& r) { return r.id == "id_b"; });
  ASSERT_NE(updated, out.end());
  EXPECT_EQ(updated->contentHash, "hash-updated");
  EXPECT_EQ(updated->sizeBytes, 999u);
}

TEST(ManifestIndexMerge, OrderingSurvivesAMergeIncludingARename) {
  // id_r moves from "/m/rename-me.epub" (a middle position in the old index) to "/b/renamed.epub" --
  // earlier than where it used to sort, but still after "/a/keep.epub" and before "/z/keep2.epub".
  // The merge must place it at its *new* sorted position, not its old one, and drop the stale copy.
  std::string index;
  index += formatIndexLine(makeRecord("id_keep", "/a/keep.epub"));
  index += formatIndexLine(makeRecord("id_r", "/m/rename-me.epub"));
  index += formatIndexLine(makeRecord("id_keep2", "/z/keep2.epub"));

  const std::vector<ManifestIndexRecord> upserts{makeRecord("id_r", "/b/renamed.epub")};

  std::vector<ManifestIndexRecord> out;
  ManifestIndexMerge merge({"id_r"}, upserts, &collectMergedRecords, &out);
  ASSERT_TRUE(merge.feed(reinterpret_cast<const uint8_t*>(index.data()), index.size()));
  ASSERT_TRUE(merge.finish());
  EXPECT_FALSE(merge.hasError());

  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0].path, "/a/keep.epub");
  EXPECT_EQ(out[1].path, "/b/renamed.epub");
  EXPECT_EQ(out[1].id, "id_r");
  EXPECT_EQ(out[2].path, "/z/keep2.epub");
}

TEST(ManifestIndexMerge, WorksWhenFedInSmallChunksAcrossLineBoundaries) {
  const std::string index = buildSampleIndex();
  std::vector<ManifestIndexRecord> out;
  ManifestIndexMerge merge({"bok_3"}, {}, &collectMergedRecords, &out);
  bool fedOk = true;
  for (size_t i = 0; i < index.size() && fedOk; ++i) {
    fedOk = merge.feed(reinterpret_cast<const uint8_t*>(index.data() + i), 1);
  }
  ASSERT_TRUE(fedOk);
  ASSERT_TRUE(merge.finish());
  EXPECT_FALSE(merge.hasError());
  EXPECT_EQ(idsOf(out), (std::vector<std::string>{"bok_1", "bok_2", "bok_4", "bok_5"}));
}

TEST(ManifestIndexMerge, ReportsErrorOnACorruptOldIndexLine) {
  const std::string corrupt = "bok_1|/path.epub|not-a-number|hash|1755300000|0\n";
  std::vector<ManifestIndexRecord> out;
  ManifestIndexMerge merge({}, {}, &collectMergedRecords, &out);
  EXPECT_FALSE(merge.feed(reinterpret_cast<const uint8_t*>(corrupt.data()), corrupt.size()));
  EXPECT_TRUE(merge.hasError());
}

}  // namespace
