#include <gtest/gtest.h>

#include <algorithm>

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

// --- The watermark ----------------------------------------------------------
// See RecentDiscovery.h: novelty is "updatedAt beyond the mark", not "absent
// from the list", because a list smaller than the manifest evicts entries
// that then read as new again -- the reshuffle bug these tests pin.

std::vector<std::string> pathsOf(const std::vector<ListEntry>& list) {
  std::vector<std::string> out;
  for (const ListEntry& entry : list) out.push_back(entry.path);
  return out;
}

// Simulates what HomeActivity::runRecentDiscovery() + RecentBooksStore do
// with a decision: insert most-recent-first at the front, then trim to the
// remote cap. Oldest-first push so the newest ends up leading, matching the
// real caller's backwards walk over insertFront.
void applyInsertions(std::vector<ListEntry>& list, const std::vector<ManifestView>& insertFront, size_t remoteCap) {
  for (auto it = insertFront.rbegin(); it != insertFront.rend(); ++it) {
    list.insert(list.begin(), listEntry(it->path, it->id));
  }
  if (list.size() > remoteCap) list.resize(remoteCap);
}

TEST(RecentDiscoveryWatermark, DecideAppliedThenRunAgainstTheSameManifestInsertsNothingTheSecondTime) {
  Input in;
  in.manifest = {
      manifestView("m1", "/books/a.epub", 100),
      manifestView("m2", "/books/b.epub", 300),
      manifestView("m3", "/books/c.epub", 200),
  };
  in.maxInsert = 5;

  const Result first = decide(in);
  ASSERT_EQ(first.insertFront.size(), 3u);
  EXPECT_GT(first.newWatermark, 0u);

  std::vector<ListEntry> current;
  applyInsertions(current, first.insertFront, /*remoteCap=*/5);

  Input second = in;
  second.current = current;
  second.discoveredWatermark = first.newWatermark;

  const Result again = decide(second);
  EXPECT_TRUE(again.insertFront.empty());
  EXPECT_TRUE(again.dropRemoteIds.empty());
  EXPECT_EQ(again.newWatermark, first.newWatermark);
}

TEST(RecentDiscoveryWatermark, EightBookLibraryWithARemoteCapOfFiveStabilizesAndStaysStableAcrossThreeBoots) {
  constexpr size_t kRemoteCap = 5;
  Input in;
  in.manifest = {
      manifestView("m1", "/books/b1.epub", 100), manifestView("m2", "/books/b2.epub", 200),
      manifestView("m3", "/books/b3.epub", 300), manifestView("m4", "/books/b4.epub", 400),
      manifestView("m5", "/books/b5.epub", 500), manifestView("m6", "/books/b6.epub", 600),
      manifestView("m7", "/books/b7.epub", 700), manifestView("m8", "/books/b8.epub", 800),
  };
  in.maxInsert = kRemoteCap;

  std::vector<ListEntry> current;
  uint64_t watermark = 0;

  // Boot 1: eight books compete for five slots. Only this boot inserts
  // anything -- the newest five, b4..b8.
  Input boot1 = in;
  boot1.current = current;
  boot1.discoveredWatermark = watermark;
  const Result r1 = decide(boot1);
  ASSERT_EQ(r1.insertFront.size(), kRemoteCap);
  watermark = r1.newWatermark;
  applyInsertions(current, r1.insertFront, kRemoteCap);
  const std::vector<std::string> stablePaths = pathsOf(current);
  EXPECT_EQ(stablePaths, (std::vector<std::string>{"/books/b8.epub", "/books/b7.epub", "/books/b6.epub",
                                                   "/books/b5.epub", "/books/b4.epub"}));

  // Boots 2 and 3: same manifest, nothing changed on the server. Each must
  // insert nothing and drop nothing -- not merely "the same order", which
  // the broken code also produced on its second boot; it only diverged on
  // the third (see docs/superpowers/sdd task-1-brief.md).
  for (int boot = 2; boot <= 3; boot++) {
    Input next = in;
    next.current = current;
    next.discoveredWatermark = watermark;
    const Result r = decide(next);
    EXPECT_TRUE(r.insertFront.empty()) << "boot " << boot;
    EXPECT_TRUE(r.dropRemoteIds.empty()) << "boot " << boot;
    EXPECT_EQ(r.newWatermark, watermark) << "boot " << boot;
    watermark = r.newWatermark;
    applyInsertions(current, r.insertFront, kRemoteCap);
    EXPECT_EQ(pathsOf(current), stablePaths) << "boot " << boot;
  }

  // The three oldest never make it in at all -- a deliberate trade (see
  // RecentDiscovery.h), not a bug: they are reachable through the file
  // browser and the download queue instead.
  for (const char* neverSeen : {"/books/b1.epub", "/books/b2.epub", "/books/b3.epub"}) {
    EXPECT_EQ(std::find(stablePaths.begin(), stablePaths.end(), neverSeen), stablePaths.end());
  }
}

TEST(RecentDiscoveryWatermark, ARecordNewerThanTheMarkIsStillInsertedAfterTheListHasSettled) {
  Input in;
  in.manifest = {
      manifestView("m1", "/books/a.epub", 100),
      manifestView("m2", "/books/b.epub", 200),
  };
  in.current = {listEntry("/books/a.epub", "m1")};
  in.discoveredWatermark = 100;

  const Result result = decide(in);
  ASSERT_EQ(result.insertFront.size(), 1u);
  EXPECT_EQ(result.insertFront[0].id, "m2");
  EXPECT_EQ(result.newWatermark, 200u);
}

TEST(RecentDiscoveryWatermark, ARecordAtOrBelowTheMarkIsNeverReinsertedEvenWhenAbsentFromTheList) {
  Input in;
  in.manifest = {manifestView("m1", "/books/a.epub", 100)};
  in.discoveredWatermark = 100;  // exactly at the mark

  const Result atMark = decide(in);
  EXPECT_TRUE(atMark.insertFront.empty());

  in.discoveredWatermark = 150;  // below the mark
  const Result belowMark = decide(in);
  EXPECT_TRUE(belowMark.insertFront.empty());
}

TEST(RecentDiscoveryWatermark, SeveralNewRecordsAboveTheMarkAreStillOrderedByUpdatedAtDescendingTiesByPathAscending) {
  Input in;
  in.discoveredWatermark = 50;
  in.manifest = {
      manifestView("m1", "/books/z.epub", 100),
      manifestView("m2", "/books/a.epub", 100),
      manifestView("m3", "/books/mid.epub", 300),
  };

  const Result result = decide(in);
  ASSERT_EQ(result.insertFront.size(), 3u);
  EXPECT_EQ(result.insertFront[0].id, "m3");
  EXPECT_EQ(result.insertFront[1].path, "/books/a.epub");
  EXPECT_EQ(result.insertFront[2].path, "/books/z.epub");
}

TEST(RecentDiscoveryWatermark, FirstSyncStillSeedsAWatermarkEvenThoughItInsertsNothing) {
  Input in;
  in.manifest = {
      manifestView("m1", "/books/a.epub", 100),
      manifestView("m2", "/books/b.epub", 200),
      manifestView("m3", "/books/c.epub", 300),
  };
  in.firstSync = true;

  const Result result = decide(in);
  EXPECT_TRUE(result.insertFront.empty());
  EXPECT_EQ(result.newWatermark, 300u);
}

TEST(RecentDiscoveryWatermark, NewWatermarkCoversRecordsExcludedFromInsertionNotJustTheOnesInserted) {
  Input in;
  // Already in the list -- excluded from insertion by the double-insertion
  // guard -- but still a book-like manifest record with a real updatedAt.
  in.manifest = {
      manifestView("m1", "/books/already-listed.epub", 999),
      manifestView("m2", "/books/new.epub", 100),
  };
  in.current = {listEntry("/books/already-listed.epub", "m1")};

  const Result result = decide(in);
  ASSERT_EQ(result.insertFront.size(), 1u);
  EXPECT_EQ(result.insertFront[0].id, "m2");
  // Computing the mark from insertFront alone would leave it at 100, and the
  // next sync -- were m1 ever to fall out of the list -- would treat it as
  // new again, which is the bug this file exists to fix.
  EXPECT_EQ(result.newWatermark, 999u);
}

TEST(RecentDiscoveryWatermark, NewWatermarkNeverRegressesBelowWhatCameIn) {
  Input in;
  in.manifest = {manifestView("m1", "/books/a.epub", 50)};
  in.discoveredWatermark = 500;  // higher than anything in this sync's manifest

  const Result result = decide(in);
  EXPECT_EQ(result.newWatermark, 500u);
  EXPECT_TRUE(result.insertFront.empty());  // 50 <= 500, not new
}

}  // namespace
