#pragma once

#include "components/themes/roundedraff/RoundedRaffTheme.h"

class GfxRenderer;

namespace SheetMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = RoundedRaffMetrics::values;
  // The continue-reading card is a selectable tile of its own at index 0, so
  // the button menu starts at index 1 -- the Lyra/Classic arrangement, not
  // RoundedRaff's folded-into-the-menu one.
  v.homeContinueReadingInMenu = false;
  v.homeRecentBooksCount = 1;
  // Epub::generateThumbBmp() hard-codes thumb width = height * 0.6, so 190
  // yields the 114x190 thumbnail the card is laid out around.
  v.homeCoverHeight = 190;
  // The thumbnail plus 10px of card padding above and below it.
  v.homeCoverTileHeight = 210;
  return v;
}();
}  // namespace SheetMetrics

// Sheet: RoundedRaff's list/header/popup language with two Home-screen changes
// -- a compact horizontal continue-reading card, and a two-column tile grid in
// place of the vertical button menu.
class SheetTheme : public RoundedRaffTheme {
 public:
  // RoundedRaff suppresses the header entirely when the title is null, which is
  // what HomeActivity passes for every theme with homeContinueReadingInMenu ==
  // false. Sheet goes back to the base header so the battery still renders on
  // the Home screen.
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                  const char* subtitle = nullptr) const override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
  int getMenuRowHeight(const GfxRenderer& renderer) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
};
