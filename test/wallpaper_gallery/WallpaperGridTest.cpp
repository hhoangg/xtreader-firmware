// The 3x2 grid's geometry and paging. The panel has no partial refresh, so
// "which six entries are on screen" is also "which six thumbnails must be on
// the card before anything is painted" -- which is why the paging arithmetic is
// worth pinning down away from hardware.

#include <gtest/gtest.h>

#include "WallpaperGrid.h"

namespace {

using wallpaper_grid::Bounds;
using wallpaper_grid::Layout;

// The X4 in portrait: 480x800, with the tab band above and the button hints
// below already taken out.
Bounds portraitContent() { return Bounds{20, 105, 440, 615}; }

Layout portraitLayout() { return wallpaper_grid::layout(portraitContent(), 17); }

}  // namespace

TEST(WallpaperGridLayout, FitsThreeColumnsInPortrait) {
  const Layout layout = portraitLayout();
  ASSERT_TRUE(layout.valid);
  EXPECT_EQ(layout.artWidth, wallpaper_grid::ART_WIDTH);
  EXPECT_EQ(layout.artHeight, wallpaper_grid::ART_HEIGHT);

  const Bounds first = wallpaper_grid::artBounds(layout, 0);
  const Bounds last = wallpaper_grid::artBounds(layout, wallpaper_grid::PAGE_SIZE - 1);
  const Bounds content = portraitContent();
  EXPECT_EQ(first.x, content.x);
  EXPECT_EQ(first.y, content.y);
  EXPECT_LE(last.x + last.width, content.x + content.width);
  EXPECT_LE(wallpaper_grid::tileBounds(layout, wallpaper_grid::PAGE_SIZE - 1).y +
                wallpaper_grid::tileBounds(layout, wallpaper_grid::PAGE_SIZE - 1).height,
            content.y + content.height);
}

TEST(WallpaperGridLayout, PlacesSixSlotsInReadingOrder) {
  const Layout layout = portraitLayout();
  ASSERT_TRUE(layout.valid);
  for (int row = 0; row < wallpaper_grid::ROWS; row++) {
    for (int column = 0; column < wallpaper_grid::COLUMNS; column++) {
      const Bounds tile = wallpaper_grid::tileBounds(layout, row * wallpaper_grid::COLUMNS + column);
      EXPECT_EQ(tile.x, layout.originX + column * layout.columnStep);
      EXPECT_EQ(tile.y, layout.originY + row * layout.rowStep);
    }
  }
}

TEST(WallpaperGridLayout, ColumnsNeverOverlap) {
  const Layout layout = portraitLayout();
  ASSERT_TRUE(layout.valid);
  EXPECT_GT(layout.columnStep, layout.artWidth);
  EXPECT_GT(layout.rowStep, layout.tileHeight);
}

// Landscape (800x480) has far less vertical room; the art has to shrink rather
// than run off the band, and it must keep its aspect ratio while doing so.
TEST(WallpaperGridLayout, ShrinksTheArtRatherThanOverflowing) {
  const Bounds content{20, 105, 760, 300};
  const Layout layout = wallpaper_grid::layout(content, 17);
  ASSERT_TRUE(layout.valid);
  EXPECT_LT(layout.artHeight, wallpaper_grid::ART_HEIGHT);
  EXPECT_LT(layout.artWidth, wallpaper_grid::ART_WIDTH);

  // Aspect ratio preserved to within a pixel of rounding.
  const int expectedWidth = layout.artHeight * wallpaper_grid::ART_WIDTH / wallpaper_grid::ART_HEIGHT;
  EXPECT_NEAR(layout.artWidth, expectedWidth, 1);

  const Bounds last = wallpaper_grid::tileBounds(layout, wallpaper_grid::PAGE_SIZE - 1);
  EXPECT_LE(last.y + last.height, content.y + content.height);
  EXPECT_LE(last.x + last.width, content.x + content.width);
}

TEST(WallpaperGridLayout, ReportsAnUnusableBand) {
  EXPECT_FALSE(wallpaper_grid::layout(Bounds{0, 0, 0, 0}, 17).valid);
  EXPECT_FALSE(wallpaper_grid::layout(Bounds{0, 0, 440, 60}, 17).valid);
  EXPECT_FALSE(wallpaper_grid::layout(Bounds{0, 0, 60, 615}, 17).valid);

  // An invalid layout yields empty rects rather than garbage coordinates.
  const Bounds tile = wallpaper_grid::tileBounds(wallpaper_grid::layout(Bounds{0, 0, 0, 0}, 17), 0);
  EXPECT_EQ(tile.width, 0);
  EXPECT_EQ(tile.height, 0);
}

TEST(WallpaperGridLayout, IgnoresAnOutOfRangeSlot) {
  const Layout layout = portraitLayout();
  EXPECT_EQ(wallpaper_grid::tileBounds(layout, -1).width, 0);
  EXPECT_EQ(wallpaper_grid::tileBounds(layout, wallpaper_grid::PAGE_SIZE).width, 0);
}

// --- Paging -----------------------------------------------------------------

TEST(WallpaperGridPaging, AnchorsThePageToTheSelection) {
  EXPECT_EQ(wallpaper_grid::pageStart(0), 0);
  EXPECT_EQ(wallpaper_grid::pageStart(5), 0);
  EXPECT_EQ(wallpaper_grid::pageStart(6), 6);
  EXPECT_EQ(wallpaper_grid::pageStart(11), 6);
  EXPECT_EQ(wallpaper_grid::pageStart(12), 12);
  EXPECT_EQ(wallpaper_grid::pageStart(-3), 0);

  EXPECT_EQ(wallpaper_grid::slotOf(0), 0);
  EXPECT_EQ(wallpaper_grid::slotOf(7), 1);
  EXPECT_EQ(wallpaper_grid::pageIndex(7), 1);
  EXPECT_EQ(wallpaper_grid::pageIndex(-1), 0);
}

TEST(WallpaperGridPaging, CountsPagesAndPartialLastPages) {
  EXPECT_EQ(wallpaper_grid::pageCount(0), 0);
  EXPECT_EQ(wallpaper_grid::pageCount(1), 1);
  EXPECT_EQ(wallpaper_grid::pageCount(6), 1);
  EXPECT_EQ(wallpaper_grid::pageCount(7), 2);

  EXPECT_EQ(wallpaper_grid::slotsOnPage(0, 4), 4);
  EXPECT_EQ(wallpaper_grid::slotsOnPage(0, 20), 6);
  EXPECT_EQ(wallpaper_grid::slotsOnPage(6, 8), 2);
  EXPECT_EQ(wallpaper_grid::slotsOnPage(12, 8), 0);
  EXPECT_EQ(wallpaper_grid::slotsOnPage(0, 0), 0);
}

TEST(WallpaperGridPaging, FetchesTheNextPageBeforePaintingAHalfEmptyOne) {
  // Six loaded, more available, selection moves onto page 1: fetch first.
  EXPECT_TRUE(wallpaper_grid::needsNextPage(6, 6, /*hasMore=*/true));
  // Same, but the server said that was everything: paint what there is.
  EXPECT_FALSE(wallpaper_grid::needsNextPage(6, 6, /*hasMore=*/false));
  // Page fully backed already.
  EXPECT_FALSE(wallpaper_grid::needsNextPage(0, 6, /*hasMore=*/true));
  EXPECT_FALSE(wallpaper_grid::needsNextPage(5, 12, /*hasMore=*/true));
  // A partially filled current page still wants the rest of itself.
  EXPECT_TRUE(wallpaper_grid::needsNextPage(0, 4, /*hasMore=*/true));
}
