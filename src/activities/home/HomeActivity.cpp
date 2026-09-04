#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FreeInkUIGfxRenderer.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <RecentDiscovery.h>
#include <SyncTriggerPolicy.h>
#include <Utf8.h>
#include <WiFi.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "SleepWifiBackoffPolicy.h"
#include "SyncCredentialStore.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/folder.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"
#include "components/icons/xtreader.h"
#include "fontIds.h"
#include "sync/BookFinishedNotifier.h"
#include "sync/DownloadQueue.h"
#include "sync/SleepProgressSync.h"
#include "sync/SyncManifest.h"
#include "sync/Telemetry.h"
#include "sync/WallpaperSync.h"

namespace {
// Once-per-boot latches for trySyncLibrary(): HomeActivity is destroyed and
// recreated every time the library screen is (re-)entered (goHome() calls
// ActivityManager::replaceActivity()), so a member flag would reset on every
// visit; these plain statics survive across those instances and reset only
// on a real reboot -- which this device also goes through on every sleep
// wake (see SyncTriggerPolicy.h), so "once per boot" and "once per wake"
// are the same event here.
bool manifestSyncAttemptedThisBoot = false;
// Gates the Wi-Fi bring-up itself (see shouldAttemptLibraryWifiConnect()),
// separate from manifestSyncAttemptedThisBoot above: a device that is
// already connected on its first Home visit never needs a bring-up attempt
// at all, but must still gate the sync itself the usual way.
bool libraryWifiConnectAttemptedThisBoot = false;

// Set when a Wi-Fi bring-up this boot actually connected, and cleared once the
// manifest sync that follows has reported whether the network was really
// reachable. It has to outlive a single render pass because those two steps are
// now deliberately split across passes -- see trySyncLibrary().
bool libraryWifiBringUpAwaitingSyncResult = false;

// Once-per-boot latch for trySyncWallpapers(), separate from the library
// one: this counts the boot toward the wallpaper cadence even on a boot that
// does not sync (see CrossPointState::bootsSinceWallpaperSync), so it has to
// be set on every path, not only the one that reaches the network.
bool wallpaperSyncCheckedThisBoot = false;

// The opaque wallpaper-set fingerprint this boot's heartbeat came back with
// (0 = none, see telemetry::TelemetryResult::wallpaperRevision). Set by
// trySyncLibrary(), read by trySyncWallpapers() -- which usually runs on a
// *later* render pass than the heartbeat that filled this in, since it
// defers whenever trySyncLibrary() drew a popup. A plain static for the same
// reason as the latches above: it has to outlive both this render pass and
// this HomeActivity instance, but never a reboot.
uint32_t heartbeatWallpaperRevision = 0;

// Set by trySyncLibrary() whenever it puts a popup on screen this render
// pass. drawPopup() paints only its own box, sized to its own text, and the
// requestUpdate() that follows is deferred to the end of
// ActivityManager::loop() -- so a second popup drawn in the same pass lands
// inside the first one's, whose edges stay visible around it.
// trySyncWallpapers() reads this and defers to the next pass rather than
// drawing over it, the same hand-off trySyncLibrary() already makes to
// itself after a Wi-Fi bring-up.
bool librarySyncPopupUsedThisPass = false;

// Context for the wallpaper sync's progress callback -- plain pointers, not
// a capturing lambda (see CLAUDE.md's "Template and std::function Bloat").
struct WallpaperProgressCtx {
  GfxRenderer* renderer;
  Rect popup;
};

void onWallpaperSyncProgress(void* ctxPtr, const uint32_t done, const uint32_t total) {
  if (total == 0) return;
  auto* ctx = static_cast<WallpaperProgressCtx*>(ctxPtr);
  GUI.fillPopupProgress(*ctx->renderer, ctx->popup, static_cast<int>(done * 100 / total));
}

// --- The band under the cover tile ------------------------------------------
//
// Three book rows and the icon nav strip, drawn straight onto the immediate-
// mode screen through a one-slot FreeInkUI frame -- the same path
// BaseTheme::drawHeader() and WallpaperGalleryActivity::drawChrome() take. The
// theme tokens supply the text styles, ThemeMetrics supplies every length, and
// nothing below allocates beyond a stack buffer: the largest contiguous heap
// block falls to a few KB while a download is in flight, and a repaint that
// cannot allocate aborts the device rather than failing.

namespace fui = freeink::ui;

// The nav strip's five destinations, in drawing order. Xtreader sits in the
// middle: it is this fork's own screen, and the middle cell is the one a thumb
// reaches without looking.
constexpr size_t NAV_ITEM_COUNT = 5;
// Intrinsic size of the legacy menu icon assets (components/icons/folder.h and
// friends). Not a layout choice: they exist at exactly this size.
constexpr int NAV_ICON_SIZE = 32;
// The row's left cell: a cover-shaped box 3/4 as tall as its row, in the 40:54
// proportion the tile's own thumbnails use.
constexpr int SLOT_ART_HEIGHT_NUM = 3;
constexpr int SLOT_ART_HEIGHT_DEN = 4;
constexpr int SLOT_ART_ASPECT_NUM = 40;
constexpr int SLOT_ART_ASPECT_DEN = 54;
// Selection geometry, mirroring SheetTheme's kCardSelectInset / kCardSelectRadius
// / kThumbRadius so the row's selected state and the cover tile's read as one
// widget rather than two. The frame is drawn OUTSIDE the content column, which
// is what puts clear air between the border and the artwork -- drawing it at
// the content's own edge makes the two touch.
constexpr int SLOT_SELECT_OUTSET = 6;
constexpr uint8_t SLOT_SELECT_RADIUS = 14;
constexpr uint8_t SLOT_ART_RADIUS = 9;

fui::Rect r16(const int x, const int y, const int w, const int h) {
  return fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w), static_cast<int16_t>(h)};
}

// The nav strip's labels, in the same order as navIconBits() below. Shared
// with the test console's selected-row accessor so the two report one list.
const char* navLabel(const size_t index) {
  switch (index) {
    case 0:
      return tr(STR_BROWSE_FILES);
    case 1:
      return tr(STR_MENU_RECENT_BOOKS);
    case 2:
      return tr(STR_XTREADER);
    case 3:
      return tr(STR_FILE_TRANSFER);
    default:
      return tr(STR_SETTINGS_TITLE);
  }
}

const uint8_t* navIconBits(const size_t index) {
  switch (index) {
    case 0:
      return FolderIcon;
    case 1:
      return RecentIcon;
    case 2:
      return XtreaderIcon;
    case 3:
      return TransferIcon;
    default:
      return Settings2Icon;
  }
}

// GfxRenderer::drawIcon always plots black. A selected nav slot is filled, so
// its glyph has to come out in paper instead -- same asset, same pre-rotated
// (size-1-row, col) mapping, opposite ink.
void drawNavIcon(const GfxRenderer& renderer, const uint8_t* bitmap, const int x, const int y, const bool ink) {
  constexpr int rowBytes = (NAV_ICON_SIZE + 7) / 8;
  for (int row = 0; row < NAV_ICON_SIZE; row++) {
    for (int col = 0; col < NAV_ICON_SIZE; col++) {
      const uint8_t byte = bitmap[row * rowBytes + (col >> 3)];
      if (((byte >> (7 - (col & 7))) & 1) == 0) {
        renderer.drawPixel(x + (NAV_ICON_SIZE - 1 - row), y + col, ink);
      }
    }
  }
}

void formatByteSize(const uint64_t bytes, char* out, const size_t outSize) {
  if (bytes >= 1024 * 1024) {
    snprintf(out, outSize, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else {
    snprintf(out, outSize, "%.0f KB", static_cast<double>(bytes) / 1024.0);
  }
}

// The row's third line: what this book is doing, in the one place the reader
// can see it. Written into caller storage so no row builds a std::string.
void formatSlotStatus(const home_book_slots::Slot& slot, char* out, const size_t outSize) {
  switch (slot.state) {
    case home_book_slots::State::OnServer: {
      char size[16];
      formatByteSize(slot.sizeBytes, size, sizeof(size));
      snprintf(out, outSize, "%s - %s", tr(STR_HOME_ON_SERVER), size);
      break;
    }
    case home_book_slots::State::Queued:
      // The position matters because the worker downloads one book at a time;
      // it is 0 only for a queue entry that arrived without one.
      if (slot.queuePosition > 0) {
        snprintf(out, outSize, tr(STR_HOME_QUEUE_POSITION), slot.queuePosition);
      } else {
        snprintf(out, outSize, "%s", tr(STR_HOME_QUEUED));
      }
      break;
    case home_book_slots::State::Downloading:
      snprintf(out, outSize, "%s", tr(STR_HOME_DOWNLOADING_SHORT));
      break;
    case home_book_slots::State::Failed:
      snprintf(out, outSize, "%s", tr(STR_HOME_DOWNLOAD_FAILED));
      break;
    case home_book_slots::State::JustDownloaded:
      snprintf(out, outSize, "%s", tr(STR_HOME_NOT_STARTED));
      break;
    case home_book_slots::State::Read:
      if (slot.progressPercent > 0) {
        snprintf(out, outSize, tr(STR_HOME_READ_PERCENT), slot.progressPercent);
      } else {
        snprintf(out, outSize, "%s", tr(STR_HOME_NOT_STARTED));
      }
      break;
  }
}

// The pre-dithered thumbnail the tile already caches, reused verbatim and
// scaled down by the renderer. False when this book has none: only the tile's
// own book is guaranteed one (see loadRecentCovers()).
bool drawCoverThumb(const GfxRenderer& renderer, const fui::Rect cell, const std::string& coverBmpPath) {
  if (coverBmpPath.empty()) return false;
  const std::string path =
      UITheme::getCoverThumbPath(coverBmpPath, UITheme::getInstance().getMetrics().homeCoverHeight);
  HalFile file;
  if (!Storage.openFileForRead("HOME", path, file)) return false;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;
  renderer.drawBitmap(bitmap, cell.x, cell.y, cell.width, cell.height);
  return true;
}

// The left cell. A remote book has no cover and will not have one until it is
// downloaded, so its cell carries the download state as a shape instead.
void drawSlotArt(const GfxRenderer& renderer, fui::GfxRendererTarget& target, const fui::Rect cell,
                 const home_book_slots::Slot& slot, const fui::ThemeTokens& tokens) {
  const int heavy = std::max<int>(2, tokens.spaceXs);
  // Mirrors SheetTheme's kThumbRadius, so the placeholder that stands in for a
  // cover is the same shape as the cover it will become, and as the rounded
  // selection frame around the row.
  const uint8_t artRadius = tokens.listRowRadius != 0 ? tokens.listRowRadius : SLOT_ART_RADIUS;

  switch (slot.state) {
    case home_book_slots::State::OnServer: {
      // Dithered rather than dashed. A dash pattern cannot follow a rounded
      // corner without special-casing the arcs, and the dither already carries
      // the "lighter than queued" reading on its own. Two pixels thick, not
      // one: a dither is a checkerboard, so whether a one-pixel run has any ink
      // depends on the parity of its y, and an even-height cell put the top and
      // bottom edges on opposite parities -- observed on hardware as a box
      // missing its bottom edge.
      target.stroke(cell, fui::Paint::dither(fui::Color::LightGray), 2, artRadius);
      const fui::BitmapRef arrow = fui::bitmapFromIcon(icon_download_24);
      const int glyph = std::min<int>(arrow.width, std::min(cell.width, cell.height));
      target.bitmap(r16(cell.x + (cell.width - glyph) / 2, cell.y + (cell.height - glyph) / 2, glyph, glyph), arrow,
                    fui::BitmapMode::Contain, fui::Paint::solid(fui::Color::Black));
      break;
    }
    case home_book_slots::State::Queued:
      target.stroke(cell, fui::Paint::solid(fui::Color::Black), static_cast<uint8_t>(heavy), artRadius);
      break;
    case home_book_slots::State::Downloading:
      target.fill(cell, fui::Paint::solid(fui::Color::Black), artRadius);
      break;
    case home_book_slots::State::Failed: {
      target.stroke(cell, fui::Paint::solid(fui::Color::Black), static_cast<uint8_t>(heavy), artRadius);
      // A bang, built from two rects rather than typed: it has to read at this
      // size in every font the UI can be running, including the CJK fallback.
      const int barW = heavy;
      const int barH = cell.height / 3;
      const int barX = cell.x + (cell.width - barW) / 2;
      const int barY = cell.y + (cell.height - barH - barW * 3) / 2;
      target.fill(r16(barX, barY, barW, barH), fui::Paint::solid(fui::Color::Black));
      target.fill(r16(barX, barY + barH + barW * 2, barW, barW), fui::Paint::solid(fui::Color::Black));
      break;
    }
    case home_book_slots::State::JustDownloaded:
    case home_book_slots::State::Read:
      if (!drawCoverThumb(renderer, cell, slot.coverBmpPath)) {
        target.fill(cell, fui::Paint::dither(fui::Color::LightGray), artRadius);
      }
      target.stroke(cell, fui::Paint::solid(fui::Color::Black), 1, artRadius);
      break;
  }
}

void drawSlotRow(const GfxRenderer& renderer, fui::GfxRendererTarget& target, const fui::ThemeTokens& tokens,
                 const fui::Rect row, const home_book_slots::Slot& slot, const bool selected, const bool divider,
                 const int sidePadding) {
  if (row.empty()) return;
  if (divider) {
    target.fill(r16(row.x + sidePadding, row.y, row.width - sidePadding * 2, 1),
                fui::Paint::dither(fui::Color::LightGray));
  }

  int artH = std::min(row.height * SLOT_ART_HEIGHT_NUM / SLOT_ART_HEIGHT_DEN, row.height - tokens.spaceSm * 2);
  int artW = artH * SLOT_ART_ASPECT_NUM / SLOT_ART_ASPECT_DEN;
  // The text is the row; the art never takes more than a third of it.
  const int maxArtW = (row.width - sidePadding * 2) / 3;
  if (artW > maxArtW) {
    artW = maxArtW;
    artH = artW * SLOT_ART_ASPECT_DEN / SLOT_ART_ASPECT_NUM;
  }
  if (artW <= 0 || artH <= 0) return;
  const fui::Rect art = r16(row.x + sidePadding, row.y + (row.height - artH) / 2, artW, artH);

  if (selected) {
    // The frame is the art box grown by the same amount on all four sides, then
    // stretched to the content column's width. Deriving the vertical inset from
    // a constant instead left the top and bottom noticeably thicker than the
    // sides, because the art is centred in the row and the row's height varies
    // by theme. Clamped to the row so a tall text column cannot push it out.
    // Drawn OUTSIDE the content the way SheetTheme frames the cover tile: at
    // the content's own edge the border touches the artwork, and at the
    // screen's edge the panel's painted bezel swallows the left and right
    // sides so the selection reads as two bare horizontal bars.
    const int top = std::max<int>(row.y + 1, art.y - SLOT_SELECT_OUTSET);
    const int bottom = std::min<int>(row.y + row.height - 1, art.y + art.height + SLOT_SELECT_OUTSET);
    target.stroke(
        r16(art.x - SLOT_SELECT_OUTSET, top, row.width - sidePadding * 2 + SLOT_SELECT_OUTSET * 2, bottom - top),
        fui::Paint::solid(fui::Color::Black), 1, tokens.listRowRadius != 0 ? tokens.listRowRadius : SLOT_SELECT_RADIUS);
  }
  drawSlotArt(renderer, target, art, slot, tokens);

  const int textX = art.right() + tokens.spaceMd;
  const int textW = row.right() - sidePadding - textX;
  if (textW <= 0) return;

  // Weight and size carry the hierarchy, not colour: this renderer has no
  // dithered text path, so a gray secondary line would come out solid black.
  fui::TextStyle titleStyle = tokens.bodyText;
  titleStyle.bold = true;
  fui::TextStyle authorStyle = tokens.smallText;
  fui::TextStyle statusStyle = tokens.smallText;
  // The two states the reader is waiting on carry the weight.
  statusStyle.bold = slot.state == home_book_slots::State::Downloading || slot.state == home_book_slots::State::Failed;

  const int titleH = target.lineHeight(titleStyle.font);
  const int authorH = target.lineHeight(authorStyle.font);
  const int statusH = target.lineHeight(statusStyle.font);
  int y = row.y + (row.height - titleH - authorH - statusH) / 2;

  int titleW = textW;
  if (slot.state == home_book_slots::State::JustDownloaded) {
    // The badge rides beside the title, inverted, so the book that just landed
    // is findable without reading a word. It lives one session: the board has
    // no clock, so after a sleep nothing can still say how long ago "just" was.
    const char* badge = tr(STR_HOME_JUST_DOWNLOADED);
    fui::TextStyle badgeStyle = tokens.smallText;
    badgeStyle.bold = true;
    badgeStyle.align = fui::TextAlign::Center;
    badgeStyle.inverted = true;
    const int badgeW = target.measureText(badgeStyle.font, badge, badgeStyle).width + tokens.spaceSm * 2;
    const int fits = std::min<int>(target.measureText(titleStyle.font, slot.title.c_str(), titleStyle).width,
                                   textW - badgeW - tokens.spaceSm);
    if (fits > 0) {
      titleW = fits;
      const fui::Rect badgeRect = r16(textX + titleW + tokens.spaceSm, y, badgeW, titleH);
      target.fill(badgeRect, fui::Paint::solid(fui::Color::Black), tokens.listRowRadius);
      target.text(badgeRect, badge, badgeStyle);
    }
  }

  target.text(r16(textX, y, titleW, titleH), slot.title.c_str(), titleStyle);
  y += titleH;
  target.text(r16(textX, y, textW, authorH), slot.author.c_str(), authorStyle);
  y += authorH;

  char status[96];
  formatSlotStatus(slot, status, sizeof(status));
  target.text(r16(textX, y, textW, statusH), status, statusStyle);
}

// The strip itself carries icons only, so it stays symmetric; the selected
// destination's name spans the full width on its own line ABOVE the strip's top
// rule. That line is reserved whether or not anything is selected, so nothing
// moves when the selector steps off the strip and back onto the book rows.
void drawNavBar(const GfxRenderer& renderer, fui::GfxRendererTarget& target, const fui::ThemeTokens& tokens,
                const fui::Rect label, const fui::Rect bar, const int selected) {
  const bool hasSelection = selected >= 0 && selected < static_cast<int>(NAV_ITEM_COUNT);
  if (hasSelection) {
    fui::TextStyle labelStyle = tokens.smallText;
    labelStyle.bold = true;
    labelStyle.align = fui::TextAlign::Center;
    target.text(label, navLabel(static_cast<size_t>(selected)), labelStyle);
  }

  if (bar.empty()) return;
  const int rule = tokens.headerUnderline > 0 ? tokens.headerUnderline : 1;
  target.fill(r16(bar.x, bar.y, bar.width, rule), fui::Paint::solid(fui::Color::Black));

  const int slotH = std::min<int>(NAV_ICON_SIZE + tokens.spaceSm * 2, bar.height - rule - tokens.spaceXs * 2);
  if (slotH <= 0) return;
  const int slotW = slotH + tokens.spaceMd;
  const int cellW = bar.width / static_cast<int>(NAV_ITEM_COUNT);
  const int slotY = bar.y + rule + (bar.height - rule - slotH) / 2;

  for (size_t i = 0; i < NAV_ITEM_COUNT; i++) {
    const int cellX = bar.x + cellW * static_cast<int>(i);
    const bool isSelected = static_cast<int>(i) == selected;
    if (isSelected) {
      target.fill(r16(cellX + (cellW - slotW) / 2, slotY, slotW, slotH), fui::Paint::solid(fui::Color::Black),
                  tokens.listRowRadius);
    }
    drawNavIcon(renderer, navIconBits(i), cellX + (cellW - NAV_ICON_SIZE) / 2, slotY + (slotH - NAV_ICON_SIZE) / 2,
                !isSelected);
  }
}
}  // namespace

int HomeActivity::coverSelectionCount() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // 0 on a homeContinueReadingInMenu theme: there, Continue Reading was a
  // highlighted row inside the button menu, and the tile itself was never a
  // selector position -- RoundedRaffTheme::drawRecentBookCover() takes
  // selectorIndex and ignores it, so a cursor parked on the tile would draw
  // no highlight anywhere. The menu is gone, so the tile is simply not
  // selectable there; Back still opens that book directly, and its own hint
  // already reads "Resume". Every other theme highlights one tile per
  // displayed cover (BaseTheme/Lyra on index 0, Lyra3Covers on index i).
  if (metrics.homeContinueReadingInMenu) return 0;
  return static_cast<int>(tileBooks.size());
}

int HomeActivity::slotRowCount() const {
  return static_cast<int>(std::min(slots_.size(), home_book_slots::SLOT_COUNT));
}

int HomeActivity::getMenuItemCount() const {
  // Cover tile, then one position per book row, then the five nav icons.
  return coverSelectionCount() + slotRowCount() + static_cast<int>(NAV_ITEM_COUNT);
}

int HomeActivity::slotSelectionIndex() const {
  const int index = selectorIndex - coverSelectionCount();
  return index >= 0 && index < slotRowCount() ? index : -1;
}

int HomeActivity::navSelectionIndex() const {
  const int index = selectorIndex - coverSelectionCount() - slotRowCount();
  return index >= 0 && index < static_cast<int>(NAV_ITEM_COUNT) ? index : -1;
}

#ifdef CP_TEST_CONSOLE
bool HomeActivity::getSelectedRowInfo(std::string& outLabel, int& outIndex, int& outCount) const {
  outIndex = selectorIndex;
  outCount = getMenuItemCount();

  const int nav = navSelectionIndex();
  if (nav >= 0) {
    outLabel = navLabel(static_cast<size_t>(nav));
    return true;
  }
  const int slot = slotSelectionIndex();
  if (slot >= 0) {
    outLabel = slots_[static_cast<size_t>(slot)].title;
    return true;
  }
  outLabel.clear();  // selection is on a recent-book cover tile, which has no text label
  return true;
}
#endif

HomeActivity::SlotBandLayout HomeActivity::slotBandLayout() const {
  const auto& metrics = UITheme::getInstance().getMetrics();

  SlotBandLayout layout;
  layout.x = 0;
  layout.width = renderer.getScreenWidth();
  // Top edge is where the button menu has always started; the bottom edge is
  // the hints strip, which owns the rest.
  layout.rowsTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  layout.height = renderer.getScreenHeight() - metrics.buttonHintsHeight - layout.rowsTop;
  if (layout.height <= 0 || layout.width <= 0) return layout;

  // Bottom-anchored: the strip is the theme's own menu row height, the label
  // above it is one line of the small font, and whatever is left over divides
  // evenly among the three rows. That is 310px on Classic and 446px on Lyra,
  // so nothing here can be a pixel constant. The line height comes from the
  // same font slot drawSlotBand()'s FreeInkUI target resolves FONT_SMALL to.
  layout.navBarHeight = std::min(metrics.menuRowHeight, layout.height);
  layout.navLabelHeight =
      std::min(renderer.getLineHeight(uiScaleSpec().smallFontId), layout.height - layout.navBarHeight);
  layout.navBarTop = layout.rowsTop + layout.height - layout.navBarHeight;
  layout.navLabelTop = layout.navBarTop - layout.navLabelHeight;
  layout.navCellWidth = layout.width / static_cast<int>(NAV_ITEM_COUNT);

  const int rowsHeight = layout.height - layout.navBarHeight - layout.navLabelHeight;
  layout.rowHeight = rowsHeight / static_cast<int>(home_book_slots::SLOT_COUNT);
  layout.rowCount = slotRowCount();
  return layout;
}

void HomeActivity::drawSlotBand(const SlotBandLayout& layout, const int selectedSlot, const int selectedNav) const {
  if (layout.height <= 0 || layout.width <= 0) return;
  const auto& metrics = UITheme::getInstance().getMetrics();

  const auto spec = uiScaleSpec();
  fui::GfxRendererFrame<1> ui(renderer, spec.smallFontId, spec.bodyFontId, spec.titleFontId);
  // Refresh the app-wide shared tokens rather than copying ~1.5KB of
  // ThemeTokens onto this render-path stack frame -- see UiAppHelpers.h.
  const fui::ThemeTokens& tokens = refreshSharedUiThemeTokens(ui.target);

  for (int i = 0; i < layout.rowCount && layout.rowHeight > 0; i++) {
    drawSlotRow(renderer, ui.target, tokens,
                r16(layout.x, layout.rowsTop + layout.rowHeight * i, layout.width, layout.rowHeight),
                slots_[static_cast<size_t>(i)], i == selectedSlot, /*divider=*/i > 0, metrics.contentSidePadding);
  }

  drawNavBar(renderer, ui.target, tokens, r16(layout.x, layout.navLabelTop, layout.width, layout.navLabelHeight),
             r16(layout.x, layout.navBarTop, layout.width, layout.navBarHeight), selectedNav);
}

void HomeActivity::loadRecentBooks() {
  const int tileCap = std::max(1, UITheme::getInstance().getMetrics().homeRecentBooksCount);

  recencyList.clear();
  tileBooks.clear();
  // A copy of the store's list, not a view into it: the loop below calls
  // Storage.exists() per entry, and a task that inserts while this yields
  // would reallocate the vector this is walking.
  const std::vector<RecentBook> books = RECENT_BOOKS.getBooks();
  recencyList.reserve(books.size());
  tileBooks.reserve(static_cast<size_t>(tileCap));

  for (const RecentBook& book : books) {
    // A remote entry is never missing -- it has no local file yet (see
    // RecentBooksStore::isMissing).
    if (RecentBooksStore::isMissing(book)) continue;
    recencyList.push_back(book);
    // Only a book that is actually on the card can fill a tile: a remote entry
    // has no cover and no title to draw one with.
    if (book.remoteId.empty() && tileBooks.size() < static_cast<size_t>(tileCap)) {
      tileBooks.push_back(book);
    }
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  // Only the covers actually shown in the tile. The rest of the recency list
  // has no tile to generate a thumbnail for.
  const int loadCount = static_cast<int>(tileBooks.size());
  int progress = 0;
  for (int i = 0; i < loadCount; i++) {
    RecentBook& book = tileBooks[i];
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / loadCount));
          bool success = epub.generateThumbBmp(coverHeight);
          if (!success) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / loadCount));
            bool success = xtc.generateThumbBmp(coverHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::rebuildSlots() {
  // Held for the whole function, not just the assignment at the end.
  // loadRecentBooks() below rewrites both lists, render() reads both, and
  // loadRecentCovers() writes tileBooks[i].coverBmpPath through a live reference
  // on the render task -- so without this, a queue transition landing during
  // thumbnail generation is a concurrent read and write of a std::string.
  // Safe from every call site: ActivityManager::loop() calls loop() with the
  // lock deliberately not held ("the loop() method must be responsible for
  // acquire one if needed") and unlocks before onEnter(). It is NOT safe from
  // render(), which already holds it -- see trySyncLibrary(), which sets
  // slotsRebuildPending instead of calling this.
  RenderLock lock(*this);

  // The one source, re-read here rather than at onEnter() alone: a finished
  // download clears its entry's remoteId in the store (BookDownloader.cpp's
  // markDownloaded), and a sync's discoveries are inserted there too, so
  // without this the rows would keep rendering the list as it was when this
  // activity was created.
  loadRecentBooks();

  home_book_slots::Input input;
  input.recents.reserve(recencyList.size());
  for (const RecentBook& book : recencyList) {
    input.recents.push_back(
        {book.path, book.title, book.author, book.coverBmpPath, book.progressPercent, book.remoteId, book.sizeBytes});
  }

  // One snapshot, not one call per queue entry below -- see DownloadQueue.h's
  // Snapshot comment on why this copies up to MAX_QUEUE std::strings.
  const download_queue::Snapshot queueSnap = download_queue::snapshot();
  input.queue.entries.reserve(queueSnap.count);
  for (size_t i = 0; i < queueSnap.count; i++) {
    const auto state = queueSnap.items[i].status == download_queue::ItemStatus::Downloading
                           ? home_book_slots::State::Downloading
                           : home_book_slots::State::Queued;
    input.queue.entries.push_back({queueSnap.items[i].id, state, static_cast<int>(i) + 1});
  }
  if (queueSnap.lastResult.hasResult && !queueSnap.lastResult.ok) {
    input.queue.lastFailedId = queueSnap.lastResult.id;
  }

  // One per tile the theme actually draws, not just tileBooks[0]: on
  // Lyra3Covers the band is three wide, and subtracting only the first left
  // its other two showing again as rows.
  input.coverTilePaths.reserve(tileBooks.size());
  for (const RecentBook& book : tileBooks) input.coverTilePaths.push_back(book.path);

  slots_ = home_book_slots::fill(input);

  // Under the same lock: a rebuild can shorten the index space (a finished
  // download removes its row), and render() reads selectorIndex.
  const int count = getMenuItemCount();
  if (selectorIndex >= count) selectorIndex = count > 0 ? count - 1 : 0;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  // Reads the recency list and fills both it and slots_. Before the selector
  // math below: slotRowCount() reads slots_, and the nav strip sits after the
  // book rows in the selector's index space.
  rebuildSlots();

  selectorIndex = initialMenuItem == HomeMenuItem::NONE
                      ? 0
                      : coverSelectionCount() + slotRowCount() + menuItemToIndex(initialMenuItem);

  // The rows just built already reflect the queue as it stands, so entering
  // mid-download starts in sync rather than rebuilding on the first poll for
  // a change that predates this activity.
  const download_queue::Pulse pulse = download_queue::pulse();
  lastPulseGeneration = pulse.generation;
  lastPulseCompletions = pulse.completions;

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::pollDownloadQueue() {
  // No stand-off after a popup, unlike FileBrowserActivity's poll: every
  // popup Home draws (loadRecentCovers(), trySyncLibrary(), trySyncWallpapers())
  // is a progress popup whose own code path immediately follows it with
  // requestUpdate() to wipe it. None of them is a message left standing for
  // the reader, so there is nothing here for a poll-driven repaint to erase --
  // FileBrowserActivity's guard exists for its delete-error popup, which
  // deliberately omits that requestUpdate().
  const download_queue::Pulse pulse = download_queue::pulse();
  if (pulse.generation == lastPulseGeneration && pulse.completions == lastPulseCompletions) return;

  // pulse().progress is deliberately not read: it ticks once per HTTP chunk,
  // and a repaint mid-transfer has 5-7 KB of contiguous heap to run in.
  lastPulseGeneration = pulse.generation;
  lastPulseCompletions = pulse.completions;

  // A finished download needs nothing tracked here: BookDownloader has
  // already cleared the entry's remoteId in the store, keeping its position,
  // and the re-read inside rebuildSlots() picks that up.
  rebuildSlots();  // takes RenderLock itself, and releases it before the update below
  requestUpdate();
}

void HomeActivity::showEnqueueRefused(const download_queue::EnqueueOutcome outcome) {
  // Nothing was enqueued, so the queue generation never moves and the poll
  // never repaints: without this the reader selects a row and the device is
  // simply inert. AlreadyQueued is the one refusal that needs no message --
  // the row already says "Queued".
  if (outcome == download_queue::EnqueueOutcome::AlreadyQueued) return;

  const char* message = nullptr;
  switch (outcome) {
    case download_queue::EnqueueOutcome::NotPaired:
      message = tr(STR_HOME_ENQUEUE_NOT_PAIRED);
      break;
    case download_queue::EnqueueOutcome::Full:
      message = tr(STR_HOME_ENQUEUE_QUEUE_FULL);
      break;
    default:
      // NotFound: a stale id, normally a book deleted server-side since the
      // last sync. "Try syncing again" is the actionable half of that.
      message = tr(STR_HOME_ENQUEUE_UNAVAILABLE);
      break;
  }

  // GUI.drawPopup() touches the framebuffer the render task also draws into,
  // and this runs on the UI task -- same RenderLock the file browser's own
  // in-place popups take (see performServerDeleteThenLocal()).
  RenderLock lock(*this);
  GUI.drawPopup(renderer, message);
  // Deliberately no requestUpdate(): the next input-driven redraw clears it,
  // which is what keeps the message on screen long enough to read. The
  // download poll cannot wipe it either -- nothing was enqueued, so its two
  // counters did not move.
}

void HomeActivity::activateSlot(const home_book_slots::Slot& slot) {
  switch (slot.state) {
    case home_book_slots::State::OnServer:
    case home_book_slots::State::Failed: {
      // No repaint here: enqueue() bumps the queue generation and
      // pollDownloadQueue() picks it up on the next pass, so the row's new
      // status arrives through one mechanism rather than two.
      const download_queue::EnqueueOutcome outcome = download_queue::enqueue(slot.id);
      if (outcome != download_queue::EnqueueOutcome::Ok) {
        LOG_DBG("HOME", "Enqueue of %s refused (outcome=%d)", slot.id.c_str(), static_cast<int>(outcome));
        showEnqueueRefused(outcome);
      }
      break;
    }
    case home_book_slots::State::JustDownloaded:
    case home_book_slots::State::Read:
      onSelectBook(slot.path);
      break;
    case home_book_slots::State::Queued:
    case home_book_slots::State::Downloading:
      // Nothing. Only download_queue::cancelAll() exists, so a per-row cancel
      // here would silently throw away the reader's other queued books too.
      break;
  }
}

void HomeActivity::loop() {
  if (slotsRebuildPending) {
    // The library sync that ran on the render task found the index changed;
    // this is the first moment a rebuild can take RenderLock without
    // deadlocking against it. See trySyncLibrary().
    slotsRebuildPending = false;
    rebuildSlots();
    requestUpdate();
  }

  pollDownloadQueue();  // before input: a queue move repaints whatever the reader is doing

  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int coverCount = coverSelectionCount();
  const int rowCount = slotRowCount();

  auto activateSelection = [this, coverCount, rowCount] {
    if (selectorIndex < coverCount) {
      onSelectBook(tileBooks[selectorIndex].path);
      return;
    }
    const int slotIndex = selectorIndex - coverCount;
    if (slotIndex < rowCount) {
      activateSlot(slots_[static_cast<size_t>(slotIndex)]);
      return;
    }
    switch (indexToMenuItem(slotIndex - rowCount)) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::RECENTS:
        onRecentsOpen();
        break;
      case HomeMenuItem::XTREADER:
        onXtreaderOpen();
        break;
      case HomeMenuItem::FILE_TRANSFER:
        onFileTransferOpen();
        break;
      case HomeMenuItem::SETTINGS_MENU:
        onSettingsOpen();
        break;
      default:
        break;
    }
  };

  // The cursor is one cycle. The index space runs cover tile -> book rows ->
  // nav strip and then round to the start, so every move is a modular step
  // over menuCount -- which is recomputed each pass above, because rows
  // appear and disappear as the queue moves. Right on the last icon lands on
  // position 0 (the cover tile, or the first book row on a theme where the
  // tile is not selectable, see coverSelectionCount()); backwards from
  // position 0 lands on the last icon.
  //
  // `moved` keeps a single press to a single position. NavNext/NavPrevious
  // fold the front Left/Right pair into the vertical commands (see
  // MappedInputManager::mapButton), so one physical press can reach two of
  // the registrations below; the first to act closes the rest for this pass.
  bool moved = false;
  const auto moveNext = [this, menuCount, &moved] {
    if (moved) return;
    moved = true;
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  };
  const auto movePrevious = [this, menuCount, &moved] {
    if (moved) return;
    moved = true;
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  };

  if (navSelectionIndex() >= 0) {
    // On the strip the front pair drives the same cycle horizontally, so
    // vertical movement is the side buttons alone -- binding both to
    // NavNext/NavPrevious here is exactly what would double-step. The hints
    // relabel to match; see render().
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, moveNext);
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, movePrevious);
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, moveNext);
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, movePrevious);
  } else {
    buttonNavigator.onNext(moveNext);
    buttonNavigator.onPrevious(movePrevious);
  }

  // Through the same two moves as the buttons, so a swipe travels the one
  // cycle and wraps at both ends exactly as they do.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    moveNext();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    movePrevious();
    return;
  }

  // Back is otherwise unused on the home menu: open the most recently read
  // book directly (tileBooks is most-recent-first, holds only books that are
  // on the SD card, and is already pruned of files missing from it).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && !tileBooks.empty()) {
    onSelectBook(tileBooks[0].path);
    return;
  }

  const int coverColumnCount = std::max(1, metrics.homeRecentBooksCount);
  const int coverColumnWidth = (renderer.getScreenWidth() - 2 * metrics.contentSidePadding) / coverColumnCount;
  // Normally one cell per selector position the tile owns. Where the tile is
  // not selectable (coverCount == 0, see coverSelectionCount()) it stays
  // tappable as a single cell that opens its book outright -- there is no
  // highlight to move, so a touch-down has nothing to show and only the tap
  // acts.
  const bool tileSelectable = coverCount > 0;
  const int coverCellCount = tileSelectable ? coverCount : (tileBooks.empty() ? 0 : 1);
  int touchedBook = -1;
  const auto coverTouch = mappedInput.colTouch(
      touchedBook, metrics.contentSidePadding, coverColumnWidth, coverCellCount, metrics.homeTopPadding,
      metrics.homeTopPadding + metrics.homeCoverTileHeight, tileSelectable ? coverColumnWidth : 0);
  if (coverTouch != MappedInputManager::RowTouch::None) {
    if (!tileSelectable) {
      if (coverTouch == MappedInputManager::RowTouch::Tap) onSelectBook(tileBooks[0].path);
      return;
    }
    if (coverTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedBook) {
        selectorIndex = touchedBook;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedBook;
      activateSelection();
    }
    return;
  }

  // The band's own geometry, from the same helper drawSlotBand() lays out
  // with: three stacked book rows, then five equal nav cells along the strip.
  // Nothing below recomputes a length of its own, so a tap cannot land
  // somewhere other than what was drawn.
  const SlotBandLayout band = slotBandLayout();

  int touchedRow = -1;
  const auto rowTouch = mappedInput.rowTouch(touchedRow, band.rowsTop, band.rowHeight, band.rowCount);
  if (rowTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex = coverCount + touchedRow;
    if (rowTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedIndex;
      activateSelection();
    }
    return;
  }

  int touchedNav = -1;
  const auto navTouch = mappedInput.colTouch(touchedNav, band.x, band.navCellWidth, static_cast<int>(NAV_ITEM_COUNT),
                                             band.navBarTop, band.navBarTop + band.navBarHeight, band.navCellWidth);
  if (navTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex = coverCount + rowCount + touchedNav;
    if (navTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedIndex;
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  // Band spans topPadding..homeTopPadding: the cover tile starts at the fixed
  // homeTopPadding, so the height must shrink by topPadding or the band (and a
  // centered title, e.g. RoundedRaff's book title) sinks into the tile.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding - metrics.topPadding},
                 metrics.homeContinueReadingInMenu && !tileBooks.empty() ? tileBooks[0].title.c_str() : nullptr);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = 0;
  coverRectY = metrics.homeTopPadding;
  coverRectW = pageWidth;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight}, tileBooks,
                          selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  // Everything from just below the tile down to the button hints: the book
  // rows, then the nav strip. loop()'s touch grid hit-tests the same layout.
  const int selectedNav = navSelectionIndex();
  drawSlotBand(slotBandLayout(), slotSelectionIndex(), selectedNav);

  // On the nav strip the front pair moves left/right along it instead of
  // up/down through the list, so only the words change; mapLabels() still
  // places them, and the hardware order Back/Confirm/Left/Right is fixed.
  const bool onNavStrip = selectedNav >= 0;
  const auto labels = mappedInput.mapLabels(tileBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT),
                                            onNavStrip ? tr(STR_DIR_LEFT) : tr(STR_DIR_UP),
                                            onNavStrip ? tr(STR_DIR_RIGHT) : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
    // Unconditional: loadRecentCovers() only requests an update when it had
    // to generate a thumbnail, so on the common path -- every cover already
    // cached -- nothing scheduled the next pass and the branch below (the
    // automatic library sync) was never reached at all.
    requestUpdate();
  } else {
    librarySyncPopupUsedThisPass = false;
    trySyncLibrary();
    tryDeliverPendingBookFinished();
    trySyncWallpapers();
  }
}

namespace {

// --- Post-sync discovery ---------------------------------------------------
// The device-only half of lib/RecentDiscovery: it gathers what decide() needs
// out of the on-SD manifest index, which is the part that costs I/O. The rule
// itself lives in that library and is host-tested; nothing here re-decides
// anything.

// One manifest row that survived the scan below, carrying only what the
// decision and the insert need. Not ManifestIndexRecord: its contentHash is
// ~64 bytes of dead weight per row held for the length of a whole index scan.
struct DiscoveryCandidate {
  std::string id;
  std::string path;
  uint64_t sizeBytes = 0;
  uint64_t updatedAt = 0;
};

struct DiscoveryScanCtx {
  // 0 disables candidate collection entirely -- and with it the SD stat at
  // the bottom of the callback. What the first sync after pairing uses: it
  // inserts nothing whatever the manifest holds, so paying ~30 stats to
  // build a candidate list decide() will throw away is pure cost.
  size_t maxCandidates = 0;
  const std::vector<recent_discovery::ListEntry>* current = nullptr;
  std::vector<DiscoveryCandidate>* candidates = nullptr;
  std::vector<std::string>* presentRemoteIds = nullptr;
};

// The file types the recency list can actually open (RecentBooksStore::
// getDataFromBook() handles exactly these four). The manifest's own
// "looks like a book" verdict, kept here rather than in lib/RecentDiscovery,
// which deliberately has no concept of what a book file looks like.
bool looksLikeBookPath(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const std::string_view name = std::string_view{path}.substr(slash == std::string::npos ? 0 : slash + 1);
  return FsHelpers::hasEpubExtension(name) || FsHelpers::hasXtcExtension(name) || FsHelpers::hasTxtExtension(name) ||
         FsHelpers::hasMarkdownExtension(name);
}

// listByPrefix("/", ...) callback for runRecentDiscovery(): one pass over the
// index fills both of decide()'s manifest-side inputs -- the capped candidate
// list, and which of the list's remote ids the server still has.
//
// Ordered cheapest-test-first for the same reason SyncManifest.cpp's
// onTopUndownloadedCandidate is, and it is the same trick: everything above
// the last test is string comparison against the handful of entries the
// recency list holds, and the verdict at the bottom is an SD stat on a card
// that shares its SPI bus with the display. A record is only stat'd once it has beaten
// the oldest candidate kept so far, so a scan costs a handful of stats rather
// than one per book in the library.
bool onDiscoveryCandidate(void* ctxPtr, const ManifestIndexRecord& record) {
  auto* ctx = static_cast<DiscoveryScanCtx*>(ctxPtr);

  // Independent of the candidate tests below, and deliberately first: this
  // confirms ids that ARE already in the list, which every test below skips.
  // An entry whose id stops appearing here was deleted on the server, and
  // decide() drops it.
  for (const recent_discovery::ListEntry& entry : *ctx->current) {
    if (!entry.remoteId.empty() && entry.remoteId == record.id) {
      ctx->presentRemoteIds->push_back(record.id);
      break;
    }
  }

  if (ctx->maxCandidates == 0) return true;  // first sync -- nothing will be inserted, so skip the stat
  if (record.downloaded) return true;        // free, and always false today (see SyncManifest.h)
  for (const recent_discovery::ListEntry& entry : *ctx->current) {
    if (entry.path == record.path) return true;  // already in the list, local or remote
  }
  if (!looksLikeBookPath(record.path)) return true;

  auto& out = *ctx->candidates;
  size_t pos = out.size();
  while (pos > 0 && out[pos - 1].updatedAt < record.updatedAt) pos--;
  if (pos >= ctx->maxCandidates) return true;  // older than every kept candidate -- would not make the cut

  // Only now, for a record that would actually make the cut: the manifest
  // path is the local path verbatim (BookDownloader.cpp sets destPath =
  // record.path), so a file at it is this book, already on the card.
  if (Storage.exists(record.path.c_str())) return true;

  DiscoveryCandidate candidate{record.id, record.path, record.sizeBytes, record.updatedAt};
  if (out.size() < ctx->maxCandidates) {
    out.insert(out.begin() + static_cast<std::ptrdiff_t>(pos), std::move(candidate));
  } else {
    // At the cap: shift the tail down in place and overwrite the last slot,
    // rather than insert()+pop_back(), which would transiently grow the
    // vector past its reserve().
    for (size_t i = out.size() - 1; i > pos; i--) out[i] = std::move(out[i - 1]);
    out[pos] = std::move(candidate);
  }
  return true;
}

}  // namespace

void HomeActivity::runRecentDiscovery() {
  recent_discovery::Input input;
  // The first sync against a pairing seeds the marker and inserts nothing --
  // otherwise a freshly paired device pulls the owner's whole library in as
  // "new". The marker lives with the pairing, so unlinking forgets it and a
  // re-pair behaves like a fresh device (SyncCredentialStore.h).
  input.firstSync = !SYNC_STORE.isManifestSeeded();

  const std::vector<RecentBook> books = RECENT_BOOKS.getBooks();
  input.current.reserve(books.size());
  for (const RecentBook& book : books) input.current.push_back({book.path, book.remoteId});

  std::vector<DiscoveryCandidate> candidates;
  std::vector<std::string> presentRemoteIds;
  // The list's remote budget, not its size: anything past that is trimmed
  // the moment it is inserted, so collecting it costs an SD stat in the scan
  // and an SD write of the whole list on insert, for nothing.
  const size_t maxCandidates = input.firstSync ? 0 : static_cast<size_t>(RecentBooksStore::MAX_REMOTE_RECENT_BOOKS);
  input.maxInsert = maxCandidates;
  candidates.reserve(maxCandidates);
  presentRemoteIds.reserve(input.current.size());
  DiscoveryScanCtx ctx{maxCandidates, &input.current, &candidates, &presentRemoteIds};

  // "/" matches every record -- the server always sends an absolute path --
  // so this is one whole-index scan, in small fixed-size chunks. Runs even on
  // a first sync: the removal rule still applies, and a remote entry left
  // over from a previous pairing is exactly what it has to clear.
  if (!sync_manifest::listByPrefix("/", &onDiscoveryCandidate, &ctx)) {
    LOG_ERR("HOME", "Recent discovery: index scan failed, leaving the list untouched");
    return;
  }

  input.manifest.reserve(candidates.size() + presentRemoteIds.size());
  for (const DiscoveryCandidate& candidate : candidates) {
    input.manifest.push_back({candidate.id, candidate.path, candidate.updatedAt, /*looksLikeBook=*/true});
  }
  // Entries already in the list, carried with their real path so decide()
  // recognises them as present rather than re-inserting them. Their updatedAt
  // is never read: only a record that can be inserted is ever sorted, and
  // these never can be. Without them the scan's cap -- which excludes every
  // already-listed record -- would read as "deleted on the server".
  for (const recent_discovery::ListEntry& entry : input.current) {
    if (entry.remoteId.empty()) continue;
    if (std::find(presentRemoteIds.begin(), presentRemoteIds.end(), entry.remoteId) == presentRemoteIds.end()) continue;
    input.manifest.push_back({entry.remoteId, entry.path, 0, true});
  }

  const recent_discovery::Result decision = recent_discovery::decide(input);

  // Drops first: they free room in a capped list before the inserts below
  // compete for it.
  for (const std::string& id : decision.dropRemoteIds) {
    for (const recent_discovery::ListEntry& entry : input.current) {
      if (entry.remoteId != id) continue;
      LOG_DBG("HOME", "Recent discovery: dropping %s, gone from the server", entry.path.c_str());
      RECENT_BOOKS.removeByPath(entry.path);
      break;
    }
  }

  // Walked backwards: insertFront is most-recent-first and addRemoteBook()
  // inserts at position 0, so the oldest has to go in first for the most
  // recent to end up leading the list.
  for (size_t i = decision.insertFront.size(); i > 0; i--) {
    const recent_discovery::ManifestView& view = decision.insertFront[i - 1];
    uint64_t sizeBytes = 0;
    for (const DiscoveryCandidate& candidate : candidates) {
      if (candidate.id == view.id) {
        sizeBytes = candidate.sizeBytes;
        break;
      }
    }
    LOG_DBG("HOME", "Recent discovery: new book %s", view.path.c_str());
    RECENT_BOOKS.addRemoteBook(view.id, view.path, sizeBytes);
  }

  // Only once the scan actually completed, so a failed one seeds nothing and
  // the next sync still gets its one quiet pass.
  SYNC_STORE.setManifestSeeded();

  LOG_DBG("HOME", "Recent discovery: %u inserted, %u dropped (firstSync=%s)", (unsigned)decision.insertFront.size(),
          (unsigned)decision.dropRemoteIds.size(), input.firstSync ? "yes" : "no");
  // This activity's own copy of the list is refreshed by the rebuild
  // trySyncLibrary() has already scheduled through slotsRebuildPending:
  // rebuildSlots() re-reads RECENT_BOOKS under its own RenderLock, which this
  // function -- running on the render task with the rendering mutex held --
  // cannot take itself.
}

void HomeActivity::trySyncLibrary() {
  bool wifiConnected = WiFi.status() == WL_CONNECTED;

  // The bring-up itself: once per boot, only when paired and not already
  // connected (see SyncTriggerPolicy.h's shouldAttemptLibraryWifiConnect()
  // for why this is now worth doing -- nothing else in a production build
  // ever connects WiFi, so without this the automatic sync below never runs
  // at all). libraryWifiBringUpAwaitingSyncResult tracks whether a bring-up
  // this boot actually connected, so the back-off update after the sync only
  // fires for an attempt this function made, not for WiFi that happened to
  // already be up for some unrelated reason.
  if (sync_trigger::shouldAttemptLibraryWifiConnect(SYNC_STORE.isPaired(), wifiConnected,
                                                    libraryWifiConnectAttemptedThisBoot)) {
    libraryWifiConnectAttemptedThisBoot = true;

    const sleep_wifi_backoff::State backoffState = sleep_progress_sync::loadWifiBackoffState();
    if (!sleep_wifi_backoff::shouldAttempt(backoffState)) {
      LOG_DBG("HOME", "Skipping library WiFi bring-up: backed off (%u skip(s) left after %u consecutive failure(s))",
              backoffState.skipsRemaining, backoffState.consecutiveFailures);
      sleep_progress_sync::saveWifiBackoffState(sleep_wifi_backoff::afterSkippedAttempt(backoffState));
    } else {
      // Visible while it happens (task brief): same blocking-popup pattern
      // the sync below already uses, reusing the existing "Connecting to
      // saved Wi-Fi..." string from the Wi-Fi selection screen rather than
      // adding a near-duplicate one. Runs from render(), already on the
      // render task -- no RenderLock needed here, same reasoning as the
      // sync popup below.
      librarySyncPopupUsedThisPass = true;
      GUI.drawPopup(renderer, tr(STR_CONNECTING_SAVED_WIFI));
      bool cancelled = false;
      // This runs on the render task, inside HomeActivity::render(), which
      // already holds ActivityManager's rendering mutex for the whole call
      // (see ActivityManager::renderTaskLoop()) -- connectToSavedWifi() must
      // not try to take it again itself, or the render task deadlocks
      // against itself (renderingMutex is not recursive). See that
      // function's header comment.
      // pollPowerButton=true keeps the owner's escape hatch on the visible,
      // popup-blocked bring-up, exactly as before the parameter existed.
      wifiConnected = sleep_progress_sync::connectToSavedWifi(cancelled, /*callerHoldsRenderLock=*/true,
                                                              /*pollPowerButton=*/true);
      requestUpdate();  // redraw Home without the popup

      if (cancelled) {
        // Power button wins, same reasoning as SleepProgressSync.cpp: leave
        // the back-off state untouched, and don't chase the sync below with
        // a search that was just deliberately cut short.
        return;
      }
      if (!wifiConnected) {
        // No network reached at all -- back off exactly as the sleep path
        // does when the search itself finds nothing (see
        // SleepWifiBackoffPolicy.h). shouldAutoSync requires wifiConnected, so
        // there is nothing left to do this pass and no second, more precise
        // "did we reach the real internet" signal coming for this attempt.
        sleep_progress_sync::saveWifiBackoffState(sleep_wifi_backoff::afterAttempt(backoffState, false));
        return;
      }

      // Hand the sync itself to the NEXT render pass instead of falling
      // through to it here.
      //
      // drawPopup() paints only its own box, sized to its own text, and the
      // requestUpdate() above is deferred -- its flag is consumed at the end of
      // ActivityManager::loop(), which cannot run while this render pass is
      // still on the stack. Drawing the "Syncing library" popup from here
      // therefore lands it on top of the wider "Connecting to saved Wi-Fi" one,
      // whose edges stay visible around it. Returning lets the next pass clear
      // the screen and repaint Home first, so the second popup opens on a clean
      // screen -- the same hand-off loadRecentCovers() already makes to this
      // function.
      //
      // The back-off update owed to this bring-up moves with it, via the flag.
      libraryWifiBringUpAwaitingSyncResult = true;
      return;
    }
  }

  if (!sync_trigger::shouldAutoSync(SYNC_STORE.isPaired(), wifiConnected, manifestSyncAttemptedThisBoot)) {
    return;
  }
  manifestSyncAttemptedThisBoot = true;

  // Visible while it happens (task brief): same blocking-popup pattern
  // loadRecentCovers() already uses above.
  librarySyncPopupUsedThisPass = true;
  GUI.drawPopup(renderer, tr(STR_SYNCING_LIBRARY));
  // Bounded, but with its own budget rather than the shorter power-off one --
  // see SyncTriggerPolicy.h's HOME_SYNC_TIMEOUT_MS for why the two differ.
  // A captive portal or black-holed server still must not stall the render
  // task behind the popup above indefinitely.
  const sync_manifest::SyncResult syncResult = sync_manifest::sync(sync_trigger::HOME_SYNC_TIMEOUT_MS);
  // FileBrowserActivity reads whatever landed on SD; syncResult itself is only used below.
  requestUpdate();  // redraw Home without the popup

  if (syncResult.ok) {
    // onEnter() built slots_ from the recency list as it was BEFORE this
    // sync, so on the one boot that actually discovers a new book Home would
    // otherwise show nothing until the reader navigated away and back -- the
    // exact problem the rows exist to solve.
    //
    // A flag, not a rebuildSlots() call: this runs on the render task inside
    // render(), which already holds the rendering mutex, and rebuildSlots()
    // takes RenderLock, which is not recursive. Calling it here deadlocks the
    // render task against itself. loop() does the rebuild and the repaint.
    slotsRebuildPending = true;

    // Merge what the sync learned into the one recency list, before that
    // rebuild runs. Touches only RECENT_BOOKS, never slots_ or this
    // activity's own copy of the list, for the reason above.
    runRecentDiscovery();
  }

  if (libraryWifiBringUpAwaitingSyncResult) {
    libraryWifiBringUpAwaitingSyncResult = false;
    // The manifest fetch above is the first real proof this bring-up
    // reached more than just the access point -- a captive portal
    // associates too, then this fetch fails exactly like "no Wi-Fi here"
    // (see SleepWifiBackoffPolicy.h's reachedNetwork() for the same
    // reasoning on the sleep path, and SyncManifest.cpp for where
    // "fetch_failed" is set). Any other error (not_paired can't happen here
    // -- paired was already checked above; sd_write_failed, corrupt_index,
    // missing_trailer, too_many_pages, rename_failed) still proves a real
    // response came back, so it must not count as "no Wi-Fi here" either.
    const bool reached = syncResult.error != "fetch_failed";
    sleep_progress_sync::saveWifiBackoffState(
        sleep_wifi_backoff::afterAttempt(sleep_progress_sync::loadWifiBackoffState(), reached));
  }

  // WiFi is already up for the manifest sync above -- one of the two moments
  // (task brief) a heartbeat can ride along without paying its own WiFi cost.
  // Best-effort: a failed heartbeat must not affect the library sync it rides
  // with, so its result is only logged, never surfaced to the reader. Same
  // automatic bound as the sync above.
  telemetry::HeartbeatInfo heartbeatInfo = telemetry::currentDeviceHeartbeatInfo();
  heartbeatInfo.lastSyncStatus = syncResult.ok ? "ok" : "failed";
  const telemetry::TelemetryResult heartbeatResult =
      telemetry::sendHeartbeat(heartbeatInfo, sync_trigger::AUTO_SYNC_TIMEOUT_MS);
  if (!heartbeatResult.ok) {
    LOG_DBG("HOME", "Heartbeat piggybacked on library sync failed (error=%s status=%d) -- diagnostics only",
            heartbeatResult.error.c_str(), heartbeatResult.httpStatus);
  }
  // The one thing the heartbeat brings back that changes behaviour: how
  // trySyncWallpapers() below learns the assigned set was edited without
  // waiting out the boot cadence. 0 on any failure, which is exactly the
  // "leave the cadence to it" value.
  heartbeatWallpaperRevision = heartbeatResult.wallpaperRevision;
}

void HomeActivity::trySyncWallpapers() {
  // Never on a pass trySyncLibrary() already drew a popup on -- see
  // librarySyncPopupUsedThisPass. Deliberately checked before the once-per-
  // boot latch below, so deferring costs a render pass, not the whole sync.
  if (librarySyncPopupUsedThisPass) return;
  if (wallpaperSyncCheckedThisBoot) return;
  wallpaperSyncCheckedThisBoot = true;

  const uint16_t boots = APP_STATE.bootsSinceWallpaperSync;
  if (!sync_trigger::shouldSyncWallpapers(SYNC_STORE.isPaired(), WiFi.status() == WL_CONNECTED,
                                          /*alreadyAttemptedThisBoot=*/false, boots, heartbeatWallpaperRevision,
                                          APP_STATE.lastSyncedWallpaperRevision)) {
    // This boot still counts toward the next sync. Saturating, and only
    // written when it actually changes -- an unpaired reader that will never
    // sync must not pay an SD write every boot forever.
    if (SYNC_STORE.isPaired() && boots < UINT16_MAX) {
      APP_STATE.bootsSinceWallpaperSync = static_cast<uint16_t>(boots + 1);
      APP_STATE.saveToFile();
    }
    return;
  }

  // Same blocking-popup pattern as trySyncLibrary() and loadRecentCovers(),
  // with loadRecentCovers()'s progress fill on top: each 96 KB wallpaper is
  // seconds of blocking, so a bar that moves is the difference between "slow"
  // and "hung". No new popup style.
  const Rect popupRect = GUI.drawPopup(renderer, tr(STR_SYNCING_WALLPAPERS));
  WallpaperProgressCtx progressCtx{&renderer, popupRect};
  // The manifest page gets the library screen's own budget; each file
  // download gets its own, longer one inside wallpaper_sync::sync().
  const wallpaper_sync::SyncResult result =
      wallpaper_sync::sync(sync_trigger::HOME_SYNC_TIMEOUT_MS, &onWallpaperSyncProgress, &progressCtx);
  requestUpdate();  // redraw Home without the popup

  if (!result.ok) {
    // Leave the counter where it is: a failed sync must not buy itself
    // another eight boots of silence.
    LOG_DBG("HOME", "Wallpaper sync failed (error=%s status=%d)", result.error.c_str(), result.httpStatus);
    return;
  }
  // Work left over (the per-sync download cap, or a file that failed) makes
  // the next boot due again instead of resetting the cadence -- see
  // SyncTriggerPolicy.h's WALLPAPER_SYNC_BOOT_INTERVAL.
  APP_STATE.bootsSinceWallpaperSync = result.moreWorkPending ? sync_trigger::WALLPAPER_SYNC_BOOT_INTERVAL : 0;
  // Recorded only on success, so a sync that never ran retries on the next
  // boot. Stored verbatim, 0 included: 0 means "we don't know which set this
  // ran against", and the next heartbeat that does know will differ from it
  // and sync again -- one redundant sync, never a missed one. It does not
  // defeat the moreWorkPending parking above either, since the cadence is an
  // independent reason to sync.
  APP_STATE.lastSyncedWallpaperRevision = heartbeatWallpaperRevision;
  APP_STATE.saveToFile();
}

void HomeActivity::tryDeliverPendingBookFinished() {
  if (!sync_trigger::shouldDeliverPendingBookFinished(!APP_STATE.pendingBookFinishedPath.empty(), SYNC_STORE.isPaired(),
                                                      WiFi.status() == WL_CONNECTED, bookFinishedAttemptedThisVisit)) {
    return;
  }
  bookFinishedAttemptedThisVisit = true;

  // No popup: unlike trySyncLibrary(), there is nothing for the owner to
  // see change, and this is a background signal, not something the reader
  // asked for -- see BookFinishedNotifier.h. Still headroom-safe to block
  // the render task briefly for, same as the sync above.
  if (book_finished_notifier::tryDeliver(APP_STATE.pendingBookFinishedPath)) {
    APP_STATE.pendingBookFinishedPath.clear();
    APP_STATE.saveToFile();
  }
  // else: leave the pending path set -- tryDeliver() already logged why,
  // and shouldDeliverPendingBookFinished() will retry on the next visit.
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onXtreaderOpen() { activityManager.goToXtreader(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }
