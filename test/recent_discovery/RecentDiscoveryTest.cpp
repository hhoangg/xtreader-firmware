#include <gtest/gtest.h>

#include "RecentDiscovery.h"

namespace {

using recent_discovery::decide;
using recent_discovery::Input;
using recent_discovery::ListEntry;
using recent_discovery::ManifestView;
using recent_discovery::Result;
using recent_discovery::shouldPrune;
using recent_discovery::TrimBudget;

ManifestView manifestView(const std::string& id, const std::string& path, uint64_t updatedAt,
                          bool looksLikeBook = true) {
  ManifestView m;
  m.id = id;
  m.path = path;
  m.updatedAt = updatedAt;
  m.looksLikeBook = looksLikeBook;
  return m;
}

ListEntry listEntry(const std::string& path, const std::string& remoteId = "") {
  ListEntry e;
  e.path = path;
  e.remoteId = remoteId;
  return e;
}

TEST(RecentDiscovery, EmptyManifestYieldsNothing) {
  Input in;
  const Result result = decide(in);
  EXPECT_TRUE(result.insertFront.empty());
  EXPECT_TRUE(result.dropRemoteIds.empty());
}

TEST(RecentDiscovery, ManifestFullyCoveredByCurrentAndDiskYieldsNothingNew) {
  Input in;
  in.manifest = {manifestView("m1", "/books/a.epub", 100), manifestView("m2", "/books/b.epub", 200)};
  in.current = {listEntry("/books/a.epub")};
  in.pathsOnDisk = {"/books/b.epub"};

  const Result result = decide(in);
  EXPECT_TRUE(result.insertFront.empty());
  EXPECT_TRUE(result.dropRemoteIds.empty());
}

TEST(RecentDiscovery, SeveralNewAreOrderedByUpdatedAtDescending) {
  Input in;
  in.manifest = {
      manifestView("m1", "/books/a.epub", 100),
      manifestView("m2", "/books/b.epub", 300),
      manifestView("m3", "/books/c.epub", 200),
  };

  const Result result = decide(in);
  ASSERT_EQ(result.insertFront.size(), 3u);
  EXPECT_EQ(result.insertFront[0].id, "m2");
  EXPECT_EQ(result.insertFront[1].id, "m3");
  EXPECT_EQ(result.insertFront[2].id, "m1");
}

TEST(RecentDiscovery, TiedUpdatedAtBreaksByPathAscending) {
  Input in;
  in.manifest = {
      manifestView("m1", "/books/z.epub", 100),
      manifestView("m2", "/books/a.epub", 100),
  };

  const Result result = decide(in);
  ASSERT_EQ(result.insertFront.size(), 2u);
  EXPECT_EQ(result.insertFront[0].path, "/books/a.epub");
  EXPECT_EQ(result.insertFront[1].path, "/books/z.epub");
}

TEST(RecentDiscovery, RecordAlreadyInCurrentByPathIsNotReinserted) {
  Input in;
  in.manifest = {manifestView("m1", "/books/a.epub", 100)};
  in.current = {listEntry("/books/a.epub", "m1")};

  const Result result = decide(in);
  EXPECT_TRUE(result.insertFront.empty());
}

TEST(RecentDiscovery, RecordAlreadyOnDiskIsNotInserted) {
  Input in;
  in.manifest = {manifestView("m1", "/books/a.epub", 100)};
  in.pathsOnDisk = {"/books/a.epub"};

  const Result result = decide(in);
  EXPECT_TRUE(result.insertFront.empty());
}

TEST(RecentDiscovery, RecordThatDoesNotLookLikeABookIsNotInserted) {
  Input in;
  in.manifest = {manifestView("m1", "/books/a.epub", 100, /*looksLikeBook=*/false)};

  const Result result = decide(in);
  EXPECT_TRUE(result.insertFront.empty());
}

TEST(RecentDiscovery, FirstSyncInsertsNothingEvenWithAFullManifest) {
  Input in;
  in.manifest = {
      manifestView("m1", "/books/a.epub", 100),
      manifestView("m2", "/books/b.epub", 200),
      manifestView("m3", "/books/c.epub", 300),
  };
  in.firstSync = true;

  const Result result = decide(in);
  EXPECT_TRUE(result.insertFront.empty());
}

TEST(RecentDiscovery, RemoteEntryNoLongerInManifestIsDropped) {
  Input in;
  in.manifest = {manifestView("m1", "/books/a.epub", 100)};
  in.current = {listEntry("/books/a.epub", "m1"), listEntry("/books/gone.epub", "m2")};

  const Result result = decide(in);
  ASSERT_EQ(result.dropRemoteIds.size(), 1u);
  EXPECT_EQ(result.dropRemoteIds[0], "m2");
}

TEST(RecentDiscovery, LocalCurrentEntryMissingFromManifestIsNotDropped) {
  Input in;
  in.manifest = {};
  in.current = {listEntry("/books/local.epub")};  // no remoteId: a local book, untouched by discovery

  const Result result = decide(in);
  EXPECT_TRUE(result.dropRemoteIds.empty());
}

TEST(RecentDiscovery, DropAppliesEvenOnFirstSync) {
  Input in;
  in.manifest = {};
  in.current = {listEntry("/books/gone.epub", "m1")};
  in.firstSync = true;

  const Result result = decide(in);
  ASSERT_EQ(result.dropRemoteIds.size(), 1u);
  EXPECT_EQ(result.dropRemoteIds[0], "m1");
  EXPECT_TRUE(result.insertFront.empty());
}

TEST(RecentDiscovery, ShouldPruneIsFalseForARemoteEntryRegardlessOfDisk) {
  EXPECT_FALSE(shouldPrune(/*hasRemoteId=*/true, /*existsOnDisk=*/false));
  EXPECT_FALSE(shouldPrune(/*hasRemoteId=*/true, /*existsOnDisk=*/true));
}

TEST(RecentDiscovery, ShouldPruneIsTrueForALocalEntryNotOnDisk) {
  EXPECT_TRUE(shouldPrune(/*hasRemoteId=*/false, /*existsOnDisk=*/false));
}

TEST(RecentDiscovery, ShouldPruneIsFalseForALocalEntryOnDisk) {
  EXPECT_FALSE(shouldPrune(/*hasRemoteId=*/false, /*existsOnDisk=*/true));
}

// --- Insertion cap ---------------------------------------------------------

TEST(RecentDiscovery, MaxInsertKeepsTheNewestAndDropsTheRest) {
  Input in;
  in.manifest = {
      manifestView("m1", "/books/a.epub", 100), manifestView("m2", "/books/b.epub", 500),
      manifestView("m3", "/books/c.epub", 400), manifestView("m4", "/books/d.epub", 300),
      manifestView("m5", "/books/e.epub", 200),
  };
  in.maxInsert = 2;

  const Result result = decide(in);
  ASSERT_EQ(result.insertFront.size(), 2u);
  EXPECT_EQ(result.insertFront[0].id, "m2");
  EXPECT_EQ(result.insertFront[1].id, "m3");
}

TEST(RecentDiscovery, MaxInsertLargerThanTheManifestChangesNothing) {
  Input in;
  in.manifest = {manifestView("m1", "/books/a.epub", 100), manifestView("m2", "/books/b.epub", 200)};
  in.maxInsert = 10;

  const Result result = decide(in);
  ASSERT_EQ(result.insertFront.size(), 2u);
  EXPECT_EQ(result.insertFront[0].id, "m2");
}

TEST(RecentDiscovery, MaxInsertZeroInsertsNothing) {
  Input in;
  in.manifest = {manifestView("m1", "/books/a.epub", 100)};
  in.maxInsert = 0;

  const Result result = decide(in);
  EXPECT_TRUE(result.insertFront.empty());
}

TEST(RecentDiscovery, MaxInsertDoesNotSuppressRemovals) {
  Input in;
  in.manifest = {manifestView("m1", "/books/a.epub", 100)};
  in.current = {listEntry("/books/gone.epub", "m9")};
  in.maxInsert = 0;

  const Result result = decide(in);
  EXPECT_TRUE(result.insertFront.empty());
  ASSERT_EQ(result.dropRemoteIds.size(), 1u);
  EXPECT_EQ(result.dropRemoteIds[0], "m9");
}

// --- The two trim budgets --------------------------------------------------

struct Entry {
  std::string name;
  bool isRemote = false;
};

// What RecentBooksStore::trimLocked() does with the budget: one forward walk
// over a front-ordered list, dropping every entry the budget refuses.
std::vector<Entry> trim(std::vector<Entry> list, size_t localCap, size_t remoteCap) {
  TrimBudget budget{localCap, remoteCap};
  std::vector<Entry> kept;
  for (Entry& entry : list) {
    if (budget.keep(entry.isRemote)) kept.push_back(entry);
  }
  return kept;
}

std::vector<std::string> names(const std::vector<Entry>& list) {
  std::vector<std::string> out;
  for (const Entry& entry : list) out.push_back(entry.name);
  return out;
}

TEST(RecentTrim, AListWithinBothCapsIsUntouched) {
  const std::vector<Entry> list = {{"r1", true}, {"l1", false}, {"r2", true}, {"l2", false}};
  EXPECT_EQ(names(trim(list, 10, 5)), names(list));
}

TEST(RecentTrim, LocalEntriesAreEvictedOldestFirstAmongThemselves) {
  const std::vector<Entry> list = {{"l1", false}, {"l2", false}, {"l3", false}};
  EXPECT_EQ(names(trim(list, 2, 5)), (std::vector<std::string>{"l1", "l2"}));
}

TEST(RecentTrim, RemoteEntriesAreEvictedOldestFirstAmongThemselves) {
  const std::vector<Entry> list = {{"r1", true}, {"r2", true}, {"r3", true}};
  EXPECT_EQ(names(trim(list, 10, 2)), (std::vector<std::string>{"r1", "r2"}));
}

TEST(RecentTrim, RemoteEntriesDoNotCountAgainstTheLocalBudget) {
  // Ten locals interleaved with remotes: every local survives a cap of ten,
  // because the remotes ahead of them are counted separately.
  std::vector<Entry> list;
  for (int i = 0; i < 10; i++) {
    list.push_back({"r" + std::to_string(i), true});
    list.push_back({"l" + std::to_string(i), false});
  }
  const std::vector<Entry> kept = trim(list, 10, 5);

  int locals = 0;
  for (const Entry& entry : kept) {
    if (!entry.isRemote) locals++;
  }
  EXPECT_EQ(locals, 10);
  EXPECT_EQ(kept.size(), 15u);
}

TEST(RecentTrim, ARemoteInsertionBurstNeverEvictsALocalEntry) {
  // The failure this rule exists for: a 92-book library, mostly not
  // downloaded, syncs once. Discovery inserts remote entries at the front,
  // each insert trimming. Under one shared cap every local entry -- and the
  // reading history it carries -- is written over. Under two budgets none is.
  std::vector<Entry> list;
  for (int i = 0; i < 10; i++) list.push_back({"local" + std::to_string(i), false});
  const std::vector<std::string> localsBefore = names(list);

  for (int i = 0; i < 40; i++) {
    list.insert(list.begin(), {"remote" + std::to_string(i), true});
    list = trim(list, 10, 5);
  }

  std::vector<std::string> localsAfter;
  int remotes = 0;
  for (const Entry& entry : list) {
    if (entry.isRemote) {
      remotes++;
    } else {
      localsAfter.push_back(entry.name);
    }
  }
  EXPECT_EQ(localsAfter, localsBefore);
  EXPECT_EQ(remotes, 5);
  // The five newest remotes, most recent first, and they lead the list.
  EXPECT_EQ(list[0].name, "remote39");
  EXPECT_EQ(list[4].name, "remote35");
}

TEST(RecentTrim, AJustInsertedRemoteSurvivesAListAlreadyFullOfLocals) {
  // The trap a smarter eviction order alone would not solve: with one shared
  // cap, a list already at the local cap has nothing to evict but the entry
  // it has just inserted.
  std::vector<Entry> list;
  for (int i = 0; i < 10; i++) list.push_back({"local" + std::to_string(i), false});
  list.insert(list.begin(), {"newRemote", true});

  const std::vector<Entry> kept = trim(list, 10, 5);
  ASSERT_FALSE(kept.empty());
  EXPECT_EQ(kept[0].name, "newRemote");
  EXPECT_EQ(kept.size(), 11u);
}

TEST(RecentTrim, ACapOfZeroDropsEveryEntryOfThatKind) {
  const std::vector<Entry> list = {{"r1", true}, {"l1", false}};
  EXPECT_EQ(names(trim(list, 10, 0)), (std::vector<std::string>{"l1"}));
  EXPECT_EQ(names(trim(list, 0, 5)), (std::vector<std::string>{"r1"}));
}

}  // namespace
