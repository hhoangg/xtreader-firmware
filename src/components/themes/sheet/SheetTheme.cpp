#include "SheetTheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/bookmark.h"
#include "components/icons/cover.h"
#include "components/icons/folder.h"
#include "components/icons/hotspot.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
#include "fontIds.h"

namespace {

// --- continue-reading card ---
constexpr int kThumbWidth = SheetMetrics::values.homeCoverHeight * 3 / 5;  // generateThumbBmp's 0.6 ratio
constexpr int kThumbRadius = 9;
constexpr int kFormatStripHeight = 18;
constexpr int kCardGap = 14;         // thumbnail to text column
constexpr int kCardSelectInset = 6;  // selection frame outside the card
constexpr int kCardSelectRadius = 14;
constexpr int kProgressBarHeight = 5;
constexpr int kProgressBarRadius = 2;
constexpr int kProgressRowGap = 5;
constexpr int kTitleFontId = NOTOSERIF_16_FONT_ID;
constexpr int kAuthorFontId = UI_10_FONT_ID;
constexpr int kProgressFontId = UI_10_FONT_ID;
constexpr int kBadgeFontId = SMALL_FONT_ID;
constexpr int kEmptyBoxSize = 84;
constexpr int kEmptyBoxRadius = 18;

// --- tile grid ---
constexpr int kTileColumns = 2;
constexpr int kTileGap = 12;
constexpr int kTileSidePadding = 16;
constexpr int kTileHeight = 210;
constexpr int kTileRadius = 14;
constexpr int kTileIconSize = 32;
constexpr int kTileIconLabelGap = 8;
constexpr int kTileLabelInset = 16;
constexpr int kTileLabelFontId = UI_12_FONT_ID;

// --- button hints (RoundedRaff geometry) ---
constexpr int kHintSidePadding = 20;
constexpr int kHintGroupGap = 10;
constexpr int kHintBottomMargin = 10;
constexpr int kHintInnerEdgePadding = 16;
constexpr int kHintFontId = SMALL_FONT_ID;

const uint8_t* menuIcon(UIIcon icon) {
  switch (icon) {
    case UIIcon::Folder:
      return FolderIcon;
    case UIIcon::Book:
      return BookIcon;
    case UIIcon::Recent:
      return RecentIcon;
    case UIIcon::Settings:
      return Settings2Icon;
    case UIIcon::Transfer:
      return TransferIcon;
    case UIIcon::Library:
      return LibraryIcon;
    case UIIcon::Wifi:
      return WifiIcon;
    case UIIcon::Hotspot:
      return HotspotIcon;
    case UIIcon::Bookmark:
      return BookmarkIcon;
    default:
      return nullptr;
  }
}

// GfxRenderer::drawIcon() always plots ink black, and a selected Sheet tile is
// filled black. Same 1bpp MSB-first layout and Portrait (size-1-row, col)
// mapping as drawIcon(), with the ink colour made a parameter.
void drawIconInk(const GfxRenderer& renderer, const uint8_t bitmap[], int x, int y, int size, bool black) {
  const int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; row++) {
    for (int col = 0; col < size; col++) {
      const uint8_t byte = bitmap[row * rowBytes + (col >> 3)];
      if (((byte >> (7 - (col & 7))) & 1) == 0) {
        renderer.drawPixel(x + (size - 1 - row), y + col, black);
      }
    }
  }
}

// Format badge across the bottom of the thumbnail. A file-format token, not
// prose: it is the same word in every language, so it is not a tr() string.
const char* formatBadge(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) return "EPUB";
  if (FsHelpers::hasXtcExtension(path)) return "XTC";
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) return "TXT";
  return "";
}

// Whole-book reading progress, 0..100, or -1 when it is not known.
//
// Read straight off the recents entry: the reader caches its own percentage
// there on exit, so nothing here touches the SD card. It is unknown for a book
// that has not been opened since the entry was written, and the card then draws
// neither the row nor the bar rather than showing a number that is not real.
int bookProgressPercent(const RecentBook& book) {
  return (book.progressPercent < 0 || book.progressPercent > 100) ? -1 : book.progressPercent;
}

}  // namespace

void SheetTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  BaseTheme::drawHeader(renderer, rect, title, subtitle);
}

int SheetTheme::getMenuRowHeight(const GfxRenderer&) const { return kTileHeight; }

void SheetTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                     const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                     bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const int cardX = rect.x + SheetMetrics::values.contentSidePadding;
  const int cardWidth = rect.width - 2 * SheetMetrics::values.contentSidePadding;
  const int cardHeight = SheetMetrics::values.homeCoverHeight;
  const int cardY = rect.y + (rect.height - cardHeight) / 2;

  if (recentBooks.empty()) {
    const int lineHeight = renderer.getLineHeight(kTitleFontId);
    const int subLineHeight = renderer.getLineHeight(kAuthorFontId);
    const int blockHeight = kEmptyBoxSize + 15 + lineHeight + 6 + subLineHeight;
    int y = rect.y + (rect.height - blockHeight) / 2;
    const int boxX = rect.x + (rect.width - kEmptyBoxSize) / 2;
    renderer.drawRoundedRect(boxX, y, kEmptyBoxSize, kEmptyBoxSize, 1, kEmptyBoxRadius, true);
    renderer.drawIcon(CoverIcon, boxX + (kEmptyBoxSize - kTileIconSize) / 2, y + (kEmptyBoxSize - kTileIconSize) / 2,
                      kTileIconSize);
    y += kEmptyBoxSize + 15;
    UITheme::drawCenteredText(renderer, rect, kTitleFontId, y, tr(STR_NO_OPEN_BOOK), true, EpdFontFamily::BOLD);
    y += lineHeight + 6;
    UITheme::drawCenteredText(renderer, rect, kAuthorFontId, y, tr(STR_START_READING));
    return;
  }

  const RecentBook& book = recentBooks[0];

  // The thumbnail is the only part of the card that touches the SD card, so it
  // is drawn once and then served from the tile snapshot HomeActivity keeps.
  // A failed restore forces a redraw rather than leaving a hole where the
  // artwork should be.
  if (!coverRendered || !bufferRestored) {
    bool hasCover = false;
    if (!book.coverBmpPath.empty()) {
      const std::string coverBmpPath =
          UITheme::getCoverThumbPath(book.coverBmpPath, SheetMetrics::values.homeCoverHeight);
      HalFile file;
      if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderer.drawBitmap(bitmap, cardX, cardY, kThumbWidth, cardHeight);
          renderer.maskRoundedRectOutsideCorners(cardX, cardY, kThumbWidth, cardHeight, kThumbRadius);
          hasCover = true;
        }
        file.close();
      }
    }

    if (!hasCover) {
      renderer.fillRect(cardX, cardY + cardHeight / 3, kThumbWidth, 2 * cardHeight / 3, true);
      renderer.drawIcon(CoverIcon, cardX + (kThumbWidth - kTileIconSize) / 2, cardY + cardHeight / 6, kTileIconSize);
      renderer.maskRoundedRectOutsideCorners(cardX, cardY, kThumbWidth, cardHeight, kThumbRadius);
    }

    // Format strip across the bottom of the thumbnail: white plate, hairline
    // above it, so it reads over artwork of any density.
    const char* badge = formatBadge(book.path);
    const int stripY = cardY + cardHeight - kFormatStripHeight;
    renderer.fillRect(cardX + 1, stripY, kThumbWidth - 2, kFormatStripHeight - 1, false);
    renderer.drawLine(cardX + 1, stripY, cardX + kThumbWidth - 2, stripY, true);
    if (badge[0] != '\0') {
      const int badgeWidth = renderer.getTextWidth(kBadgeFontId, badge, EpdFontFamily::BOLD);
      renderer.drawText(kBadgeFontId, cardX + (kThumbWidth - badgeWidth) / 2,
                        stripY + (kFormatStripHeight - renderer.getLineHeight(kBadgeFontId)) / 2 + 1, badge, true,
                        EpdFontFamily::BOLD);
    }
    renderer.drawRoundedRect(cardX, cardY, kThumbWidth, cardHeight, 1, kThumbRadius, true);

    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;  // only "rendered" once the snapshot succeeded
  }

  // Everything below repaints on every pass, on top of the restored thumbnail.
  if (selectorIndex == 0) {
    renderer.drawRoundedRect(cardX - kCardSelectInset, cardY - kCardSelectInset, cardWidth + 2 * kCardSelectInset,
                             cardHeight + 2 * kCardSelectInset, 2, kCardSelectRadius, true);
  }

  const int textX = cardX + kThumbWidth + kCardGap;
  const int textWidth = cardWidth - kThumbWidth - kCardGap;
  if (textWidth <= 0) {
    return;
  }

  const int percent = bookProgressPercent(book);
  const int progressLineHeight = renderer.getLineHeight(kProgressFontId);

  const int titleLineHeight = renderer.getLineHeight(kTitleFontId);
  const auto titleLines = renderer.wrappedText(kTitleFontId, book.title.c_str(), textWidth, 2, EpdFontFamily::BOLD);
  const int authorLineHeight = book.author.empty() ? 0 : renderer.getLineHeight(kAuthorFontId) + 4;
  const int headBlockHeight = static_cast<int>(titleLines.size()) * titleLineHeight + authorLineHeight;

  // Title block hugs the top of the column when a progress block is pinned to
  // the bottom; without one it centres against the thumbnail instead.
  int y = percent < 0 ? cardY + (cardHeight - headBlockHeight) / 2 : cardY + 2;
  for (const auto& line : titleLines) {
    renderer.drawText(kTitleFontId, textX, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += titleLineHeight;
  }
  if (!book.author.empty()) {
    y += 4;
    const std::string author = renderer.truncatedText(kAuthorFontId, book.author.c_str(), textWidth);
    renderer.drawText(kAuthorFontId, textX, y, author.c_str(), true);
  }

  if (percent < 0) {
    return;
  }

  const int barY = cardY + cardHeight - kProgressBarHeight;
  const int rowY = barY - kProgressRowGap - progressLineHeight;
  char percentText[8];
  snprintf(percentText, sizeof(percentText), "%d%%", percent);
  renderer.drawText(kProgressFontId, textX, rowY, percentText, true, EpdFontFamily::BOLD);

  renderer.fillRoundedRect(textX, barY, textWidth, kProgressBarHeight, kProgressBarRadius, Color::LightGray);
  const int fillWidth = std::clamp(textWidth * percent / 100, 0, textWidth);
  if (fillWidth > 0) {
    renderer.fillRoundedRect(textX, barY, fillWidth, kProgressBarHeight, kProgressBarRadius, Color::Black);
  }
}

void SheetTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                const std::function<std::string(int index)>& buttonLabel,
                                const std::function<UIIcon(int index)>& rowIcon) const {
  if (buttonCount <= 0) {
    return;
  }

  const int rows = (buttonCount + kTileColumns - 1) / kTileColumns;
  const int tileWidth = (rect.width - 2 * kTileSidePadding - (kTileColumns - 1) * kTileGap) / kTileColumns;
  if (tileWidth <= 0) {
    return;
  }

  // HomeActivity's menu rect overshoots the panel (its height subtracts the
  // header band a second time), which a single-column list never noticed. Clamp
  // against the real bottom of the safe area, or a second tile row lands under
  // the button hints.
  const int bottomLimit = renderer.getScreenHeight() - SheetMetrics::values.buttonHintsHeight;
  const int available = std::max(0, std::min(rect.y + rect.height, bottomLimit) - rect.y);
  const int tileHeight = std::min(kTileHeight, (available - (rows - 1) * kTileGap) / rows);
  if (tileHeight <= 0) {
    return;
  }

  const int labelLineHeight = renderer.getLineHeight(kTileLabelFontId);
  const int maxLabelWidth = std::max(0, tileWidth - 2 * kTileLabelInset);

  for (int i = 0; i < buttonCount; ++i) {
    const int col = i % kTileColumns;
    const int row = i / kTileColumns;
    const int tileX = rect.x + kTileSidePadding + col * (tileWidth + kTileGap);
    const int tileY = rect.y + row * (tileHeight + kTileGap);
    const bool selected = i == selectedIndex;

    if (selected) {
      renderer.fillRoundedRect(tileX, tileY, tileWidth, tileHeight, kTileRadius, Color::Black);
    } else {
      renderer.drawRoundedRect(tileX, tileY, tileWidth, tileHeight, 1, kTileRadius, true);
    }

    const uint8_t* icon = rowIcon ? menuIcon(rowIcon(i)) : nullptr;
    const int blockHeight = (icon ? kTileIconSize + kTileIconLabelGap : 0) + labelLineHeight;
    int contentY = tileY + (tileHeight - blockHeight) / 2;

    if (icon != nullptr) {
      drawIconInk(renderer, icon, tileX + (tileWidth - kTileIconSize) / 2, contentY, kTileIconSize, !selected);
      contentY += kTileIconSize + kTileIconLabelGap;
    }

    const std::string label =
        renderer.truncatedText(kTileLabelFontId, buttonLabel(i).c_str(), maxLabelWidth, EpdFontFamily::BOLD);
    const int labelWidth = renderer.getTextWidth(kTileLabelFontId, label.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(kTileLabelFontId, tileX + (tileWidth - labelWidth) / 2, contentY, label.c_str(), !selected,
                      EpdFontFamily::BOLD);
  }
}

void SheetTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                 const char* btn4) const {
  if (gpio.hasTouch()) {
    return;
  }

  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int hintHeight = SheetMetrics::values.buttonHintsHeight - 10;
  const int groupWidth = (pageWidth - kHintSidePadding * 2 - kHintGroupGap) / 2;
  const int hintY = pageHeight - hintHeight - kHintBottomMargin;
  const int textY = hintY + (hintHeight - renderer.getLineHeight(kHintFontId)) / 2;
  const int radius = hintHeight / 2;  // stadium

  const int leftGroupX = kHintSidePadding;
  const int rightGroupX = leftGroupX + groupWidth + kHintGroupGap;

  // btn1..btn4 arrive already mapped to the physical Back/Confirm/Left/Right
  // order (MappedInputManager::mapFrontLabels()); each hint must stay above its
  // own button, so a theme never reorders them.
  const char* backText = (btn1 && btn1[0] != '\0') ? btn1 : "";
  const char* selectText = (btn2 && btn2[0] != '\0') ? btn2 : "";
  const char* upText = (btn3 && btn3[0] != '\0') ? btn3 : "";
  const char* downText = (btn4 && btn4[0] != '\0') ? btn4 : "";

  // Clear the band first so nothing that strayed into it shows through.
  renderer.fillRect(leftGroupX, hintY, groupWidth, hintHeight, false);
  renderer.fillRect(rightGroupX, hintY, groupWidth, hintHeight, false);

  // 1px hairline instead of RoundedRaff's 2px: Sheet's frame already carries
  // the weight at this edge.
  renderer.drawRoundedRect(leftGroupX, hintY, groupWidth, hintHeight, 1, radius, true);
  renderer.drawRoundedRect(rightGroupX, hintY, groupWidth, hintHeight, 1, radius, true);

  const int selectWidth = renderer.getTextWidth(kHintFontId, selectText);
  const int downWidth = renderer.getTextWidth(kHintFontId, downText);

  renderer.drawText(kHintFontId, leftGroupX + kHintInnerEdgePadding, textY, backText);
  renderer.drawText(kHintFontId, leftGroupX + groupWidth - kHintInnerEdgePadding - selectWidth, textY, selectText);
  renderer.drawText(kHintFontId, rightGroupX + kHintInnerEdgePadding, textY, upText);
  renderer.drawText(kHintFontId, rightGroupX + groupWidth - kHintInnerEdgePadding - downWidth, textY, downText);

  renderer.setOrientation(origOrientation);
}
