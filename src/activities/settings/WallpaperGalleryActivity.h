#pragma once

#include <I18n.h>
#include <WallpaperGalleryFeed.h>
#include <WallpaperGrid.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Browse the server's wallpaper gallery over Wi-Fi, preview one full-screen,
// and add or remove it from this device.
//
// Three tabs: what is on this device, then the same gallery under two sort
// orders. "On Device" reads GET /wallpapers/manifest -- the endpoint that
// already answers exactly that question -- while the other two read
// GET /wallpapers/gallery; one parser covers both (see
// lib/WallpaperGallery/WallpaperGalleryFeed.h).
//
// THE RENDERING CONSTRAINT THAT SHAPES THIS WHOLE SCREEN: GfxRenderer has no
// partial-region refresh (displayWindow() is commented out at
// GfxRenderer.h:187), so the only way to put anything on the panel is a whole-
// screen displayBuffer() costing ~500 ms. Painting each thumbnail as it lands
// would be six of those per page. Instead every page is two passes: the grid
// with placeholder frames and a "n of 6" line goes out first, the six
// thumbnails are fetched behind it, and the finished grid goes out second.
// A page whose thumbnails are all already cached skips the first pass and
// costs a single refresh.
//
// Everything that can be decided without a renderer, a socket or an SD card
// lives in lib/WallpaperGallery and is host-tested (test/wallpaper_gallery):
// the grid geometry and paging, the cache's naming and eviction rules, and the
// NDJSON parsing. What is left here is the device-only glue.
class WallpaperGalleryActivity final : public Activity {
 public:
  explicit WallpaperGalleryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Every state but the two the reader can sit and look at is either mid-fetch
  // or waiting to be acknowledged.
  bool preventAutoSleep() override { return state_ != State::Grid && state_ != State::Preview; }
  bool skipLoopDelay() override { return true; }

 private:
  enum class State : uint8_t {
    WifiSelection,  // the Wi-Fi screen is on top of us
    Loading,        // fetching a page of rows
    Grid,           // the 3x2 grid (thumbsPending_ marks the placeholder pass)
    Preview,        // one wallpaper, full screen
    Error,          // errorMessage_, Back returns / Confirm retries
  };

  // Rows per request. Two screens' worth, so paging forward one screen usually
  // costs no request at all, while a single response stays small enough that
  // its parse never holds more than one entry plus a line buffer.
  static constexpr uint32_t PAGE_LIMIT = 12;
  // Rows kept in memory at once. Cursor pagination cannot walk backwards, so
  // pages already visited have to be remembered or paging back would refetch.
  // 48 is eight screens; past that the oldest are dropped and the tab restarts
  // from the top rather than growing without bound (~9 KB at these string
  // caps -- see WallpaperGalleryFeed.h's MAX_NAME_BYTES).
  static constexpr size_t MAX_ENTRIES = 48;

  static constexpr int TAB_COUNT = 3;
  // Ring navigation, exactly as UiTabListActivity does it: position 0 is the
  // tab band, 1..N are the tiles. A button release steps one position and a
  // hold jumps a page, which is UiListActivity::navigateButtons()' scheme; the
  // tab band is what makes the tabs reachable without inventing a second one.
  static constexpr int RING_TABS = 0;

  State state_ = State::WifiSelection;
  // Popular is the default tab (approved mockup).
  wallpaper_gallery::Sort tab_ = wallpaper_gallery::Sort::Popular;
  int ring_ = 1;
  // The tile the page view is anchored to. Kept separately from ring_ so that
  // stepping onto the tab band does not yank the grid back to page one.
  int anchorTile_ = 0;

  std::vector<wallpaper_gallery::Entry> entries_;
  std::string nextCursor_;
  bool hasMore_ = false;

  // Placeholder pass in flight: the grid is drawn with empty frames and
  // thumbsReady_/thumbsNeeded_ under it.
  bool thumbsPending_ = false;
  int thumbsReady_ = 0;
  int thumbsNeeded_ = 0;

  // Back during a blocking download. Polled by the fetch callbacks, which run
  // with the render task blocked.
  bool cancelRequested_ = false;

  std::string errorMessage_;
  // The file the preview draws: either the copy already in /.sleep or the
  // scratch download. Cleared when the preview closes.
  std::string previewPath_;

  ButtonNavigator buttonNavigator;

  // --- data ------------------------------------------------------------------
  int entryCount() const { return static_cast<int>(entries_.size()); }
  // The tile under the cursor, or -1 when the tab band has the focus.
  int selectedIndex() const { return ring_ - 1; }
  // Which tile the visible page is built around: the selection when there is
  // one, otherwise the last tile that was selected.
  int anchorIndex() const;
  const wallpaper_gallery::Entry* selectedEntry() const;
  static wallpaper_gallery::Sort tabAt(int index);
  static int tabIndex(wallpaper_gallery::Sort sort);
  const char* tabLabel(int index) const;

  void reloadTab();
  void stepTab(int direction);
  bool fetchNextPage();
  // Ids on the page holding the current selection, in slot order.
  std::vector<std::string> pageIds() const;

  // --- files -----------------------------------------------------------------
  static bool isOnDevice(const std::string& id);
  static std::string thumbPathFor(const std::string& id);
  // Streams `url` into dir/fileName via a ".part" renamed into place, the same
  // discipline src/sync/WallpaperSync.cpp's downloadOne() uses. expectedSize 0
  // means "no size to check against".
  bool downloadTo(const std::string& url, const char* dir, const std::string& fileName, uint64_t expectedSize);
  void ensurePageThumbs();
  void trimThumbCache();

  // --- actions ---------------------------------------------------------------
  void onWifiSelectionComplete(bool success);
  void openPreview();
  void toggleAttachment();
  bool attachSelected(const wallpaper_gallery::Entry& entry);
  bool detachSelected(const wallpaper_gallery::Entry& entry);
  void failWith(StrId messageId);

  // --- input -----------------------------------------------------------------
  void moveRingTo(int ring);
  void navigateButtons();
  bool handleGridButtons();
  bool handlePreviewButtons();
  bool handleErrorButtons();

  // --- rendering -------------------------------------------------------------
  // The band between the tab bar and the button hints, in logical coords.
  wallpaper_grid::Bounds contentBounds() const;
  void drawChrome() const;
  void drawGrid() const;
  void drawTile(const wallpaper_grid::Layout& layout, int slot, int entryIndex) const;
  void drawPreview() const;
  void drawFooter() const;
  static std::string formatSize(uint64_t bytes);
};
