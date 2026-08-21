// The thumbnail cache's naming and eviction rules. Two properties matter and
// both are hostile-input properties: a server id must never be able to steer a
// write or a delete out of the cache directory, and nothing this module deletes
// may ever be a file somebody else put there.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "WallpaperGalleryPaths.h"
#include "WallpaperPaths.h"

namespace {

using wallpaper_gallery_paths::FileKind;

bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

std::vector<std::string> thumbNames(const std::vector<std::string>& ids) {
  std::vector<std::string> out;
  out.reserve(ids.size());
  for (const std::string& id : ids) out.push_back(wallpaper_gallery_paths::thumbNameForId(id));
  return out;
}

}  // namespace

// --- Filename rules ---------------------------------------------------------

TEST(WallpaperGalleryPaths, BuildsCanonicalThumbNameFromId) {
  EXPECT_EQ(wallpaper_gallery_paths::thumbNameForId("wlp_AbC-123_xyz"), "wpt_wlp_AbC-123_xyz.bmp");
  EXPECT_EQ(wallpaper_gallery_paths::tempNameFor("wpt_wlp_a.bmp"), "wpt_wlp_a.bmp.part");
}

TEST(WallpaperGalleryPaths, RejectsIdsThatCouldEscapeTheDirectory) {
  EXPECT_EQ(wallpaper_gallery_paths::thumbNameForId(""), "");
  EXPECT_EQ(wallpaper_gallery_paths::thumbNameForId("../../etc/passwd"), "");
  EXPECT_EQ(wallpaper_gallery_paths::thumbNameForId("wlp_a/b"), "");
  EXPECT_EQ(wallpaper_gallery_paths::thumbNameForId("wlp_a b"), "");
  EXPECT_EQ(wallpaper_gallery_paths::thumbNameForId("wlp_a.b"), "");
  EXPECT_EQ(wallpaper_gallery_paths::thumbNameForId(std::string(wallpaper_paths::MAX_ID_LEN + 1, 'a')), "");
}

// The gallery and the sync must never disagree about which ids are safe, or a
// wallpaper the sync is willing to download would be one the gallery refuses to
// cache a thumbnail for (or, far worse, the reverse).
TEST(WallpaperGalleryPaths, SharesTheSyncsIdAlphabet) {
  const char* cases[] = {"", "wlp_ok", "wlp_a/b", "wlp_a b", "wlp_a.b", "../x", "wlp_-_-_"};
  for (const char* id : cases) {
    EXPECT_EQ(wallpaper_gallery_paths::isValidId(id), wallpaper_paths::isValidId(id)) << id;
  }
}

TEST(WallpaperGalleryPaths, ClassifiesOnlyCanonicalNamesAsOurs) {
  EXPECT_EQ(wallpaper_gallery_paths::classifyFileName("wpt_wlp_abc.bmp"), FileKind::Thumb);
  EXPECT_EQ(wallpaper_gallery_paths::classifyFileName("wpt_wlp_abc.bmp.part"), FileKind::ThumbTemp);
  EXPECT_EQ(wallpaper_gallery_paths::idFromThumbName("wpt_wlp_abc.bmp"), "wlp_abc");
  // A half-written file's id is of no use to the caller, which only wants to
  // delete it.
  EXPECT_EQ(wallpaper_gallery_paths::idFromThumbName("wpt_wlp_abc.bmp.part"), "");

  // Near-misses stay Unmanaged, and therefore undeletable.
  EXPECT_EQ(wallpaper_gallery_paths::classifyFileName("WPT_WLP_ABC.BMP"), FileKind::Unmanaged);
  EXPECT_EQ(wallpaper_gallery_paths::classifyFileName("wpt_.bmp"), FileKind::Unmanaged);
  EXPECT_EQ(wallpaper_gallery_paths::classifyFileName("wpt_wlp abc.bmp"), FileKind::Unmanaged);
  EXPECT_EQ(wallpaper_gallery_paths::classifyFileName("holiday.bmp"), FileKind::Unmanaged);
  EXPECT_EQ(wallpaper_gallery_paths::classifyFileName("README"), FileKind::Unmanaged);
}

// The sync's own files use "cpw_"; if the two directories were ever pointed at
// each other, neither module may claim the other's names.
TEST(WallpaperGalleryPaths, DoesNotClaimTheSyncsWallpaperNames) {
  const std::string syncName = wallpaper_paths::fileNameForId("wlp_abc");
  EXPECT_EQ(syncName, "cpw_wlp_abc.bmp");
  EXPECT_EQ(wallpaper_gallery_paths::classifyFileName(syncName), FileKind::Unmanaged);
  EXPECT_EQ(wallpaper_paths::classifyFileName("wpt_wlp_abc.bmp"), wallpaper_paths::FileKind::Unmanaged);
}

TEST(WallpaperGalleryPaths, RecognisesThePreviewScratchFile) {
  EXPECT_EQ(wallpaper_gallery_paths::classifyFileName(wallpaper_gallery_paths::PREVIEW_NAME), FileKind::Preview);
  EXPECT_EQ(wallpaper_gallery_paths::classifyFileName(
                wallpaper_gallery_paths::tempNameFor(wallpaper_gallery_paths::PREVIEW_NAME)),
            FileKind::Preview);
}

TEST(WallpaperGalleryPaths, JoinsPathsWithASingleSeparator) {
  EXPECT_EQ(wallpaper_gallery_paths::joinPath("/.crosspoint/wpthumb", "wpt_a.bmp"), "/.crosspoint/wpthumb/wpt_a.bmp");
  EXPECT_EQ(wallpaper_gallery_paths::joinPath("/.crosspoint/wpthumb/", "wpt_a.bmp"), "/.crosspoint/wpthumb/wpt_a.bmp");
}

// --- Eviction ---------------------------------------------------------------

TEST(WallpaperGalleryCache, LeavesEverybodyElsesFilesAlone) {
  const std::vector<std::string> local = {"holiday.bmp", "README", "cpw_wlp_a.bmp", ".hidden", "wpt_bad name.bmp"};
  const auto plan = wallpaper_gallery_paths::planCacheEviction(local, {}, /*maxEntries=*/0);
  EXPECT_TRUE(plan.empty());
}

TEST(WallpaperGalleryCache, DropsEveryHalfWrittenDownload) {
  std::vector<std::string> local = thumbNames({"wlp_a", "wlp_b"});
  local.push_back("wpt_wlp_c.bmp.part");
  local.push_back("holiday.bmp.part");

  const auto plan = wallpaper_gallery_paths::planCacheEviction(local, {"wlp_a", "wlp_b"}, /*maxEntries=*/10);
  EXPECT_EQ(plan.size(), 1u);
  EXPECT_TRUE(contains(plan, "wpt_wlp_c.bmp.part"));
}

TEST(WallpaperGalleryCache, KeepsTheCacheUnderTheBound) {
  const std::vector<std::string> local = thumbNames({"wlp_a", "wlp_b", "wlp_c", "wlp_d", "wlp_e"});
  const auto plan = wallpaper_gallery_paths::planCacheEviction(local, {}, /*maxEntries=*/2);
  EXPECT_EQ(plan.size(), 3u);
  // Listing order, oldest-listed first -- the X4 has no clock, so there is no
  // recency to sort by.
  EXPECT_EQ(plan[0], "wpt_wlp_a.bmp");
  EXPECT_EQ(plan[1], "wpt_wlp_b.bmp");
  EXPECT_EQ(plan[2], "wpt_wlp_c.bmp");
}

// The page about to be painted must survive the trim, or the render would find
// the file it just downloaded already deleted.
TEST(WallpaperGalleryCache, NeverEvictsWhatIsOnScreen) {
  const std::vector<std::string> local = thumbNames({"wlp_a", "wlp_b", "wlp_c", "wlp_d"});
  const auto plan = wallpaper_gallery_paths::planCacheEviction(local, {"wlp_a", "wlp_b"}, /*maxEntries=*/1);
  EXPECT_EQ(plan.size(), 2u);
  EXPECT_TRUE(contains(plan, "wpt_wlp_c.bmp"));
  EXPECT_TRUE(contains(plan, "wpt_wlp_d.bmp"));
  EXPECT_FALSE(contains(plan, "wpt_wlp_a.bmp"));
  EXPECT_FALSE(contains(plan, "wpt_wlp_b.bmp"));
}

TEST(WallpaperGalleryCache, LeavesThePreviewScratchAlone) {
  std::vector<std::string> local = thumbNames({"wlp_a"});
  local.push_back(wallpaper_gallery_paths::PREVIEW_NAME);
  const auto plan = wallpaper_gallery_paths::planCacheEviction(local, {}, /*maxEntries=*/0);
  EXPECT_EQ(plan.size(), 1u);
  EXPECT_EQ(plan[0], "wpt_wlp_a.bmp");
}

TEST(WallpaperGalleryCache, DoesNothingWhenTheCacheFitsTheBound) {
  const std::vector<std::string> local = thumbNames({"wlp_a", "wlp_b"});
  EXPECT_TRUE(wallpaper_gallery_paths::planCacheEviction(local, {}, /*maxEntries=*/2).empty());
}
