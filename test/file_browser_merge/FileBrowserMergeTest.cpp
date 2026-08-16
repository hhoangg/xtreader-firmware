#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "FileBrowserMerge.h"
#include "ManifestIndexFormat.h"

namespace {

using file_browser_merge::FolderMerge;
using file_browser_merge::MergedEntry;

ManifestIndexRecord makeRecord(const std::string& id, const std::string& path, bool downloaded = false) {
  ManifestIndexRecord r;
  r.id = id;
  r.path = path;
  r.sizeBytes = 100;
  r.contentHash = "hash";
  r.updatedAt = 1755300000;
  r.downloaded = downloaded;
  return r;
}

// Finds an entry by name, or nullptr if absent -- most assertions below only care about presence
// and remoteId, not encounter order.
const MergedEntry* findEntry(const std::vector<MergedEntry>& entries, const std::string& name) {
  for (const auto& e : entries) {
    if (e.name == name) return &e;
  }
  return nullptr;
}

// --- isRecognizedBookName ---------------------------------------------------

TEST(IsRecognizedBookName, AcceptsEveryLoadFilesExtension) {
  EXPECT_TRUE(file_browser_merge::isRecognizedBookName("Book.epub"));
  EXPECT_TRUE(file_browser_merge::isRecognizedBookName("Book.xtc"));
  EXPECT_TRUE(file_browser_merge::isRecognizedBookName("Book.xtch"));
  EXPECT_TRUE(file_browser_merge::isRecognizedBookName("Notes.txt"));
  EXPECT_TRUE(file_browser_merge::isRecognizedBookName("Notes.md"));
  EXPECT_TRUE(file_browser_merge::isRecognizedBookName("Cover.bmp"));
  EXPECT_TRUE(file_browser_merge::isRecognizedBookName("Cover.png"));
}

TEST(IsRecognizedBookName, IsCaseInsensitive) { EXPECT_TRUE(file_browser_merge::isRecognizedBookName("Book.EPUB")); }

TEST(IsRecognizedBookName, RejectsUnrecognizedExtension) {
  EXPECT_FALSE(file_browser_merge::isRecognizedBookName("Notes.pdf"));
  EXPECT_FALSE(file_browser_merge::isRecognizedBookName("noextension"));
}

// --- FolderMerge -------------------------------------------------------------

TEST(FolderMerge, NoIndexRendersALocalListingUnchanged) {
  // The "absent or empty index" case: nothing ever calls addRemoteRecord.
  FolderMerge merge("/", {"A.epub", "Sub/"}, /*includeHidden=*/false);
  const auto& entries = merge.entries();
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].name, "A.epub");
  EXPECT_TRUE(entries[0].remoteId.empty());
  EXPECT_EQ(entries[1].name, "Sub/");
  EXPECT_TRUE(entries[1].remoteId.empty());
}

TEST(FolderMerge, LocalOnlyBookIsUnaffectedByAnUnrelatedRemoteRecord) {
  FolderMerge merge("/", {"Local.epub"}, false);
  merge.addRemoteRecord(makeRecord("bok_1", "/Other.epub"));
  const auto& entries = merge.entries();
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_TRUE(findEntry(entries, "Local.epub") != nullptr);
  EXPECT_TRUE(findEntry(entries, "Local.epub")->remoteId.empty());
}

TEST(FolderMerge, RemoteOnlyBookBecomesAPlaceholder) {
  FolderMerge merge("/", {}, false);
  merge.addRemoteRecord(makeRecord("bok_1", "/Remote.epub"));
  const auto& entries = merge.entries();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].name, "Remote.epub");
  EXPECT_EQ(entries[0].remoteId, "bok_1");
}

TEST(FolderMerge, BookPresentBothLocallyAndRemotelyAppearsOnceAndNotAsAPlaceholder) {
  FolderMerge merge("/", {"Both.epub"}, false);
  merge.addRemoteRecord(makeRecord("bok_1", "/Both.epub"));
  const auto& entries = merge.entries();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].name, "Both.epub");
  EXPECT_TRUE(entries[0].remoteId.empty());  // local wins -- not a placeholder
}

TEST(FolderMerge, ServerOnlySubfolderAppearsAsANormalFolderNotAPlaceholder) {
  FolderMerge merge("/", {}, false);
  merge.addRemoteRecord(makeRecord("bok_1", "/Sub/Book.epub"));
  const auto& entries = merge.entries();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].name, "Sub/");
  EXPECT_TRUE(entries[0].remoteId.empty());  // a folder, never a placeholder itself
}

TEST(FolderMerge, MultipleRecordsUnderTheSameSubfolderAddOnlyOneFolderEntry) {
  FolderMerge merge("/", {}, false);
  merge.addRemoteRecord(makeRecord("bok_1", "/Sub/A.epub"));
  merge.addRemoteRecord(makeRecord("bok_2", "/Sub/B.epub"));
  merge.addRemoteRecord(makeRecord("bok_3", "/Sub/C.epub"));
  EXPECT_EQ(merge.entries().size(), 1u);
}

TEST(FolderMerge, SubfolderAlreadyPresentLocallyIsNotDuplicated) {
  FolderMerge merge("/", {"Sub/"}, false);
  merge.addRemoteRecord(makeRecord("bok_1", "/Sub/Book.epub"));
  EXPECT_EQ(merge.entries().size(), 1u);
}

TEST(FolderMerge, RecordMarkedDownloadedNeverBecomesAPlaceholder) {
  FolderMerge merge("/", {}, false);
  merge.addRemoteRecord(makeRecord("bok_1", "/Remote.epub", /*downloaded=*/true));
  EXPECT_TRUE(merge.entries().empty());
}

TEST(FolderMerge, UnrecognizedExtensionIsSkipped) {
  FolderMerge merge("/", {}, false);
  merge.addRemoteRecord(makeRecord("bok_1", "/notes.pdf"));
  EXPECT_TRUE(merge.entries().empty());
}

TEST(FolderMerge, RecordOutsideThePrefixIsIgnored) {
  FolderMerge merge("/Sub/", {}, false);
  merge.addRemoteRecord(makeRecord("bok_1", "/Other/Book.epub"));
  EXPECT_TRUE(merge.entries().empty());
}

TEST(FolderMerge, HiddenLeafIsSkippedUnlessIncludeHiddenIsSet) {
  FolderMerge hidden("/", {}, /*includeHidden=*/false);
  hidden.addRemoteRecord(makeRecord("bok_1", "/.hidden.epub"));
  EXPECT_TRUE(hidden.entries().empty());

  FolderMerge shown("/", {}, /*includeHidden=*/true);
  shown.addRemoteRecord(makeRecord("bok_1", "/.hidden.epub"));
  ASSERT_EQ(shown.entries().size(), 1u);
  EXPECT_EQ(shown.entries()[0].name, ".hidden.epub");
}

TEST(FolderMerge, HiddenSubfolderIsSkippedUnlessIncludeHiddenIsSet) {
  FolderMerge hidden("/", {}, /*includeHidden=*/false);
  hidden.addRemoteRecord(makeRecord("bok_1", "/.trash/Book.epub"));
  EXPECT_TRUE(hidden.entries().empty());
}

TEST(FolderMerge, NestedFolderPrefixDerivesTheImmediateChildOnly) {
  // A record two levels below folderPrefix still only surfaces the immediate child folder name.
  FolderMerge merge("/Kỹ năng/", {}, false);
  merge.addRemoteRecord(makeRecord("bok_1", "/Kỹ năng/Sub/Deep/Book.epub"));
  ASSERT_EQ(merge.entries().size(), 1u);
  EXPECT_EQ(merge.entries()[0].name, "Sub/");
}

TEST(FolderMerge, MixedFolderCombinesLocalPlaceholderAndRemoteFolderEntries) {
  FolderMerge merge("/", {"Local.epub", "LocalFolder/"}, false);
  merge.addRemoteRecord(makeRecord("bok_1", "/Local.epub"));           // already local -- no placeholder
  merge.addRemoteRecord(makeRecord("bok_2", "/RemoteOnly.epub"));      // placeholder
  merge.addRemoteRecord(makeRecord("bok_3", "/LocalFolder/X.epub"));   // folder already local -- no dup
  merge.addRemoteRecord(makeRecord("bok_4", "/RemoteFolder/Y.epub"));  // new remote folder

  const auto& entries = merge.entries();
  ASSERT_EQ(entries.size(), 4u);
  EXPECT_TRUE(findEntry(entries, "Local.epub")->remoteId.empty());
  EXPECT_EQ(findEntry(entries, "RemoteOnly.epub")->remoteId, "bok_2");
  EXPECT_NE(findEntry(entries, "LocalFolder/"), nullptr);
  EXPECT_TRUE(findEntry(entries, "LocalFolder/")->remoteId.empty());
  EXPECT_NE(findEntry(entries, "RemoteFolder/"), nullptr);
  EXPECT_TRUE(findEntry(entries, "RemoteFolder/")->remoteId.empty());
}

TEST(FolderMerge, TakeEntriesMovesOutAndEmptiesTheObject) {
  FolderMerge merge("/", {"A.epub"}, false);
  auto taken = merge.takeEntries();
  ASSERT_EQ(taken.size(), 1u);
  EXPECT_EQ(taken[0].name, "A.epub");
  EXPECT_TRUE(merge.entries().empty());
}

}  // namespace
