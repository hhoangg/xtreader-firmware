#include "WallpaperGrid.h"

#include <algorithm>

namespace wallpaper_grid {

namespace {

// Smallest art box still worth drawing. Below this a 134x223 wallpaper is an
// unreadable smudge, so the caller is told the band is unusable instead.
constexpr int MIN_ART_WIDTH = 48;
constexpr int MIN_ART_HEIGHT = 80;

// Breathing room between a tile's art and its caption, and between rows.
constexpr int CAPTION_GAP = 3;
constexpr int ROW_GAP = 10;
// Minimum horizontal gap between columns; whatever is left over past the art
// is shared out and this is the floor below which the art shrinks instead.
constexpr int MIN_COLUMN_GAP = 6;

}  // namespace

Layout layout(const Bounds& content, const int nameLineHeight) {
  Layout out;
  out.nameLineHeight = std::max(nameLineHeight, 0);

  if (content.width <= 0 || content.height <= 0) return out;

  const int captionHeight = CAPTION_GAP + NAME_LINES * out.nameLineHeight;

  // Width first: three art boxes plus two gaps must fit the band.
  const int widthForArt = content.width - (COLUMNS - 1) * MIN_COLUMN_GAP;
  if (widthForArt <= 0) return out;
  int artWidth = std::min(ART_WIDTH, widthForArt / COLUMNS);

  // Height next: two rows of (art + caption) plus one gap between them.
  const int heightForArt = content.height - ROW_GAP * (ROWS - 1) - ROWS * captionHeight;
  if (heightForArt <= 0) return out;
  int artHeight = std::min(ART_HEIGHT, heightForArt / ROWS);

  // Keep the 134:223 aspect ratio: whichever axis is the binding constraint
  // sets the scale, and the other follows.
  const int fromWidth = artWidth * ART_HEIGHT / ART_WIDTH;
  if (fromWidth <= artHeight) {
    artHeight = fromWidth;
  } else {
    artWidth = artHeight * ART_WIDTH / ART_HEIGHT;
  }

  if (artWidth < MIN_ART_WIDTH || artHeight < MIN_ART_HEIGHT) return out;

  out.valid = true;
  out.artWidth = artWidth;
  out.artHeight = artHeight;
  out.tileHeight = artHeight + captionHeight;

  // Spread the slack evenly: the columns share the leftover width, the rows
  // sit at the top of the band with their fixed gap.
  const int columnGap = (content.width - COLUMNS * artWidth) / (COLUMNS - 1);
  out.columnStep = artWidth + columnGap;
  out.rowStep = out.tileHeight + ROW_GAP;
  out.originX = content.x;
  out.originY = content.y;
  return out;
}

Bounds tileBounds(const Layout& layout, const int slot) {
  Bounds out;
  if (!layout.valid || slot < 0 || slot >= PAGE_SIZE) return out;
  const int column = slot % COLUMNS;
  const int row = slot / COLUMNS;
  out.x = layout.originX + column * layout.columnStep;
  out.y = layout.originY + row * layout.rowStep;
  out.width = layout.artWidth;
  out.height = layout.tileHeight;
  return out;
}

Bounds artBounds(const Layout& layout, const int slot) {
  Bounds out = tileBounds(layout, slot);
  if (out.width == 0) return out;
  out.height = layout.artHeight;
  return out;
}

int pageStart(const int selected) {
  if (selected <= 0) return 0;
  return selected / PAGE_SIZE * PAGE_SIZE;
}

int slotOf(const int selected) {
  if (selected <= 0) return 0;
  return selected % PAGE_SIZE;
}

int pageIndex(const int selected) {
  if (selected <= 0) return 0;
  return selected / PAGE_SIZE;
}

int pageCount(const int total) {
  if (total <= 0) return 0;
  return (total + PAGE_SIZE - 1) / PAGE_SIZE;
}

int slotsOnPage(const int selected, const int total) {
  if (total <= 0) return 0;
  const int start = pageStart(selected);
  if (start >= total) return 0;
  return std::min(PAGE_SIZE, total - start);
}

bool needsNextPage(const int selected, const int loadedCount, const bool hasMore) {
  if (!hasMore) return false;
  if (loadedCount < 0) return false;
  return pageStart(selected) + PAGE_SIZE > loadedCount;
}

}  // namespace wallpaper_grid
