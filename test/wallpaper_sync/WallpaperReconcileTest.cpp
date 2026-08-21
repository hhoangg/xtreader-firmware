// The reconciliation rule that decides which wallpapers to fetch and, far
// more importantly, which files to delete off somebody's SD card. /.sleep is
// shared with whatever the reader copied there by hand, so "never delete a
// file that is not ours" is the property most of these tests exist to pin
// down.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "WallpaperPaths.h"
#include "WallpaperReconcile.h"

namespace {

using wallpaper_paths::FileKind;
using wallpaper_reconcile::Limits;
using wallpaper_reconcile::ReconcilePlan;

std::string nameOf(const std::string& id) { return wallpaper_paths::fileNameForId(id); }

bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

Limits smallLimits(const size_t maxLocal, const size_t maxPerSync) {
  Limits limits;
  limits.maxLocal = maxLocal;
  limits.maxDownloadsPerSync = maxPerSync;
  return limits;
}

}  // namespace

// --- Filename rules ---------------------------------------------------------

TEST(WallpaperPaths, BuildsCanonicalNameFromId) {
  EXPECT_EQ(wallpaper_paths::fileNameForId("wlp_AbC-123_xyz"), "cpw_wlp_AbC-123_xyz.bmp");
  EXPECT_EQ(wallpaper_paths::tempNameFor("cpw_wlp_a.bmp"), "cpw_wlp_a.bmp.part");
}

TEST(WallpaperPaths, RejectsIdsThatCouldEscapeTheDirectory) {
  EXPECT_FALSE(wallpaper_paths::isValidId(""));
  EXPECT_FALSE(wallpaper_paths::isValidId("../../etc/passwd"));
  EXPECT_FALSE(wallpaper_paths::isValidId("wlp_a/b"));
  EXPECT_FALSE(wallpaper_paths::isValidId("wlp_a b"));
  EXPECT_FALSE(wallpaper_paths::isValidId("wlp_a.b"));
  EXPECT_FALSE(wallpaper_paths::isValidId(std::string(wallpaper_paths::MAX_ID_LEN + 1, 'a')));
  EXPECT_EQ(wallpaper_paths::fileNameForId("wlp_a/b"), "");
  EXPECT_TRUE(wallpaper_paths::isValidId("wlp_-_-_"));
  EXPECT_TRUE(wallpaper_paths::isValidId(std::string(wallpaper_paths::MAX_ID_LEN, 'a')));
}

TEST(WallpaperPaths, ClassifiesOnlyCanonicalNamesAsOurs) {
  EXPECT_EQ(wallpaper_paths::classifyFileName("cpw_wlp_abc.bmp"), FileKind::Managed);
  EXPECT_EQ(wallpaper_paths::classifyFileName("cpw_wlp_abc.bmp.part"), FileKind::ManagedTemp);
  EXPECT_EQ(wallpaper_paths::idFromFileName("cpw_wlp_abc.bmp"), "wlp_abc");
  // A half-written file's id is of no use to the reconciler, which only
  // wants to delete it.
  EXPECT_EQ(wallpaper_paths::idFromFileName("cpw_wlp_abc.bmp.part"), "");
}

TEST(WallpaperPaths, LeavesEveryNearMissAlone) {
  // Every one of these is a file the reader could plausibly have on the card.
  // None of them may ever be classified as this firmware's to delete.
  const char* userFiles[] = {
      "sunset.bmp",           // an ordinary hand-copied wallpaper
      "cpw.bmp",              // prefix-ish, but not the prefix
      "cpw_.bmp",             // the prefix with an empty id
      "CPW_wlp_abc.bmp",      // wrong case in the prefix
      "cpw_wlp_abc.BMP",      // wrong case in the extension
      "cpw_wlp_abc.png",      // right prefix, wrong format
      "cpw_wlp abc.bmp",      // a space is not in the id alphabet
      "cpw_wlp_abc.bmp.bak",  // a backup somebody made of one of ours
      "cpw_../escape.bmp",    // a traversal attempt in filename form
      ".cpw_wlp_abc.bmp",     // hidden, and not the canonical name
  };
  for (const char* name : userFiles) {
    EXPECT_EQ(wallpaper_paths::classifyFileName(name), FileKind::Unmanaged) << name;
  }
}

TEST(WallpaperPaths, JoinsWithExactlyOneSeparator) {
  EXPECT_EQ(wallpaper_paths::joinPath("/.sleep", "a.bmp"), "/.sleep/a.bmp");
  EXPECT_EQ(wallpaper_paths::joinPath("/.sleep/", "a.bmp"), "/.sleep/a.bmp");
  EXPECT_EQ(wallpaper_paths::joinPath("/", "a.bmp"), "/a.bmp");
}

// --- Reconciliation ---------------------------------------------------------

TEST(WallpaperReconcile, FetchesEverythingAssignedOnAnEmptyDirectory) {
  const ReconcilePlan plan = wallpaper_reconcile::plan({"wlp_a", "wlp_b"}, {});
  EXPECT_EQ(plan.downloadIds, (std::vector<std::string>{"wlp_a", "wlp_b"}));
  EXPECT_TRUE(plan.deleteNames.empty());
  EXPECT_FALSE(plan.moreWorkPending);
}

TEST(WallpaperReconcile, DoesNothingWhenTheCardAlreadyMatches) {
  const ReconcilePlan plan = wallpaper_reconcile::plan({"wlp_a", "wlp_b"}, {nameOf("wlp_a"), nameOf("wlp_b")});
  EXPECT_TRUE(plan.downloadIds.empty());
  EXPECT_TRUE(plan.deleteNames.empty());
  EXPECT_FALSE(plan.moreWorkPending);
}

TEST(WallpaperReconcile, DeletesWhatTheServerNoLongerAssigns) {
  const ReconcilePlan plan = wallpaper_reconcile::plan({"wlp_a"}, {nameOf("wlp_a"), nameOf("wlp_gone")});
  EXPECT_TRUE(plan.downloadIds.empty());
  EXPECT_EQ(plan.deleteNames, (std::vector<std::string>{nameOf("wlp_gone")}));
}

TEST(WallpaperReconcile, NeverDeletesAFileTheReaderPutThere) {
  // The one that would be data loss. Nothing assigned at all, so every
  // managed file goes -- and every other file stays.
  const std::vector<std::string> local = {
      nameOf("wlp_a"), "sunset.bmp", "Ha Long.bmp", "CPW_wlp_b.bmp", "cpw_wlp_c.BMP", "readme.txt", "cpw_.bmp",
  };
  const ReconcilePlan plan = wallpaper_reconcile::plan({}, local);
  EXPECT_EQ(plan.deleteNames, (std::vector<std::string>{nameOf("wlp_a")}));
  for (const std::string& name : local) {
    if (name == nameOf("wlp_a")) continue;
    EXPECT_FALSE(contains(plan.deleteNames, name)) << name;
  }
}

TEST(WallpaperReconcile, SweepsAwayAnInterruptedDownloadEvenWhenStillAssigned) {
  // A ".part" is never something SleepActivity draws and nothing resumes it,
  // so it goes regardless of whether its wallpaper is still assigned -- and
  // the wallpaper is fetched again from scratch.
  const std::string temp = wallpaper_paths::tempNameFor(nameOf("wlp_a"));
  const ReconcilePlan plan = wallpaper_reconcile::plan({"wlp_a"}, {temp});
  EXPECT_EQ(plan.deleteNames, (std::vector<std::string>{temp}));
  EXPECT_EQ(plan.downloadIds, (std::vector<std::string>{"wlp_a"}));
}

TEST(WallpaperReconcile, StopsAtThePerSyncDownloadCapAndSaysSo) {
  const ReconcilePlan plan = wallpaper_reconcile::plan({"wlp_a", "wlp_b", "wlp_c", "wlp_d"}, {}, smallLimits(10, 2));
  EXPECT_EQ(plan.downloadIds, (std::vector<std::string>{"wlp_a", "wlp_b"}));
  EXPECT_TRUE(plan.moreWorkPending);
  EXPECT_EQ(plan.droppedForLocalCap, 0u);
}

TEST(WallpaperReconcile, KeepsTheSameSubsetWhenTheAccountHasMoreThanTheCardWillHold) {
  const std::vector<std::string> assigned = {"wlp_a", "wlp_b", "wlp_c", "wlp_d"};
  const ReconcilePlan first = wallpaper_reconcile::plan(assigned, {}, smallLimits(2, 10));
  EXPECT_EQ(first.downloadIds, (std::vector<std::string>{"wlp_a", "wlp_b"}));
  EXPECT_EQ(first.droppedForLocalCap, 2u);
  EXPECT_FALSE(first.moreWorkPending);  // the rest are refused, not deferred

  // Run again with those two now on the card: nothing more to do, and
  // nothing churned.
  const ReconcilePlan second =
      wallpaper_reconcile::plan(assigned, {nameOf("wlp_a"), nameOf("wlp_b")}, smallLimits(2, 10));
  EXPECT_TRUE(second.downloadIds.empty());
  EXPECT_TRUE(second.deleteNames.empty());
}

TEST(WallpaperReconcile, DeletesAFileThatFellPastTheLocalCap) {
  // wlp_z sorts last, so a cap of 2 pushes it out of the keep set even
  // though the server still assigns it.
  const ReconcilePlan plan =
      wallpaper_reconcile::plan({"wlp_a", "wlp_b", "wlp_z"}, {nameOf("wlp_a"), nameOf("wlp_z")}, smallLimits(2, 10));
  EXPECT_EQ(plan.deleteNames, (std::vector<std::string>{nameOf("wlp_z")}));
  EXPECT_EQ(plan.downloadIds, (std::vector<std::string>{"wlp_b"}));
}

TEST(WallpaperReconcile, IgnoresRowsWithAnUnusableId) {
  const ReconcilePlan plan = wallpaper_reconcile::plan({"wlp_a", "../evil", "", "wlp_b"}, {});
  EXPECT_EQ(plan.downloadIds, (std::vector<std::string>{"wlp_a", "wlp_b"}));
}

TEST(WallpaperReconcile, CountsARepeatedRowOnce) {
  const ReconcilePlan plan = wallpaper_reconcile::plan({"wlp_a", "wlp_a", "wlp_b"}, {}, smallLimits(2, 10));
  EXPECT_EQ(plan.downloadIds, (std::vector<std::string>{"wlp_a", "wlp_b"}));
  EXPECT_EQ(plan.droppedForLocalCap, 0u);
}

TEST(WallpaperReconcile, HandlesAnAddAndARemoveInOneRun) {
  const ReconcilePlan plan = wallpaper_reconcile::plan({"wlp_new"}, {nameOf("wlp_old"), "user.bmp"});
  EXPECT_EQ(plan.deleteNames, (std::vector<std::string>{nameOf("wlp_old")}));
  EXPECT_EQ(plan.downloadIds, (std::vector<std::string>{"wlp_new"}));
}
