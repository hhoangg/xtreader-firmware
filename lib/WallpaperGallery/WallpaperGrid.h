#pragma once

#include <cstddef>
#include <cstdint>

// The gallery grid's geometry and paging arithmetic. Pure -- no renderer, no
// theme, no Arduino -- so the layout that has to survive four orientations and
// two panel sizes is host-tested (test/wallpaper_gallery) rather than only ever
// eyeballed on hardware, the same split lib/WallpaperSync keeps between its
// decisions and src/sync/WallpaperSync.cpp's glue.
//
// Six tiles per screen in three columns, from the approved mockup. That is the
// unit the whole screen is built around: it is the paging step, the thumbnail
// prefetch batch, and -- because GfxRenderer has no partial-region refresh
// (displayWindow() is commented out at GfxRenderer.h:187) -- the number of
// thumbnails that must be on the card before the finished grid is painted at
// all. See WallpaperGalleryActivity's two-pass render for why.
namespace wallpaper_grid {

constexpr int COLUMNS = 3;
constexpr int ROWS = 2;
constexpr int PAGE_SIZE = COLUMNS * ROWS;

// The thumbnail the server serves at ?variant=thumb. Nominal, not assumed:
// layout() scales the art box down when the content band cannot fit it, and
// GfxRenderer::drawBitmap does the same for the pixels, so a smaller panel or
// a landscape orientation degrades instead of overflowing.
constexpr int ART_WIDTH = 134;
constexpr int ART_HEIGHT = 223;

// Lines of text under each tile: the name wraps to at most two, then one
// shared line carries the uploader and the attach count.
constexpr int NAME_LINES = 2;
constexpr int META_LINES = 1;

// A rectangle in logical screen coordinates. Deliberately not `Rect` (that
// lives in src/components/themes/BaseTheme.h, which drags in the renderer), so
// this stays host-compilable.
struct Bounds {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct Layout {
  // False when the content band cannot hold even one usable row; the caller
  // draws a message instead of a grid.
  bool valid = false;
  int artWidth = 0;
  int artHeight = 0;
  int columnStep = 0;  // left edge to left edge
  int rowStep = 0;     // top edge to top edge
  int originX = 0;
  int originY = 0;
  int nameLineHeight = 0;
  int metaLineHeight = 0;
  // Art + caption; the tile's own height, excluding the gap below it.
  int tileHeight = 0;
};

// Fit the 3x2 grid into `content` (the band between the tab bar and the button
// hints). `nameLineHeight` / `metaLineHeight` come from the renderer's font
// metrics. The art keeps its 134:223 aspect ratio when it has to shrink, so a
// tile never distorts a wallpaper.
Layout layout(const Bounds& content, int nameLineHeight, int metaLineHeight);

// The whole tile (art plus caption) for a slot in 0..PAGE_SIZE-1.
Bounds tileBounds(const Layout& layout, int slot);
// Just the art box within that tile.
Bounds artBounds(const Layout& layout, int slot);

// --- paging ---------------------------------------------------------------
// A page is a fixed window of PAGE_SIZE entries, not a scrolling viewport: the
// panel repaints whole screens anyway, so anchoring the page to the selection
// keeps "hold to jump a page" and "which six thumbnails must be on the card"
// the same question.

// Index of the first entry on the page holding `selected`.
int pageStart(int selected);
// Which of the six slots `selected` occupies.
int slotOf(int selected);
// Zero-based page number of `selected`.
int pageIndex(int selected);
// How many pages `total` entries fill (0 for none).
int pageCount(int total);
// How many of the six slots on `selected`'s page actually have an entry.
int slotsOnPage(int selected, int total);

// True when the page holding `selected` is not fully backed by loaded entries
// and the server said there is more to fetch. Drives "fetch the next page
// before painting", so paging forward never shows a half-empty screen while a
// next cursor is still outstanding.
bool needsNextPage(int selected, int loadedCount, bool hasMore);

}  // namespace wallpaper_grid
