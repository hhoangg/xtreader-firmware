#pragma once
#include <HomeBookSlots.h>

#include <functional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct RecentBook;
struct Rect;

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  // The books the cover tile band draws: the leading LOCAL entries of the
  // recency list, capped at the theme's homeRecentBooksCount. Remote entries
  // are excluded deliberately -- one has no cover, no title and no author, so
  // a tile showing one would be a blank box, and Back (which opens
  // tileBooks[0]) would try to open a file that is not on the card.
  std::vector<RecentBook> tileBooks;
  // The one recency list, most recent first, local and remote entries
  // together -- the only source the rows below the tile are built from.
  std::vector<RecentBook> recencyList;
  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;
  // The home_book_slots rows below the cover tile, drawn by drawSlotBand().
  // Populated by rebuildSlots() -- see its comment for where each field of
  // its Input comes from.
  std::vector<home_book_slots::Slot> slots_;

  // Convert HomeMenuItem to menu index (used in onEnter)
  static int menuItemToIndex(HomeMenuItem item) {
    switch (item) {
      case HomeMenuItem::FILE_BROWSER:
        return 0;
      case HomeMenuItem::RECENTS:
        return 1;
      case HomeMenuItem::FILE_TRANSFER:
        return 2;
      case HomeMenuItem::SETTINGS_MENU:
        return 3;
      default:
        return 0;
    }
  }

  // Convert menu index to HomeMenuItem (used in loop)
  static HomeMenuItem indexToMenuItem(int idx) {
    switch (idx) {
      case 0:
        return HomeMenuItem::FILE_BROWSER;
      case 1:
        return HomeMenuItem::RECENTS;
      case 2:
        return HomeMenuItem::FILE_TRANSFER;
      case 3:
        return HomeMenuItem::SETTINGS_MENU;
      default:
        return HomeMenuItem::NONE;
    }
  }
  void onSelectBook(const std::string& path);
  void onFileBrowserOpen();
  void onRecentsOpen();
  void onSettingsOpen();
  void onFileTransferOpen();

  int getMenuItemCount() const;
  // Every length in the band between the cover tile and the button hints,
  // computed once. drawSlotBand() lays out from it and loop()'s touch grid
  // hit-tests against it, so a tap can never land somewhere other than what
  // was drawn -- nothing here is recomputed independently on either side.
  // All of it is derived from the active ThemeMetrics and the small font's
  // line height (Classic's band is 310px, Lyra's 446px), never from pixel
  // constants.
  struct SlotBandLayout {
    int x = 0;
    int width = 0;
    int height = 0;     // 0 when the theme leaves no room between tile and hints
    int rowsTop = 0;    // top edge of the first book row
    int rowHeight = 0;  // one book row; 0 when the band is too short for rows
    int rowCount = 0;   // rows actually drawn: slots_.size(), capped at SLOT_COUNT
    int navLabelTop = 0;
    int navLabelHeight = 0;
    int navBarTop = 0;
    int navBarHeight = 0;
    int navCellWidth = 0;  // one of the four equal nav cells
  };
  SlotBandLayout slotBandLayout() const;
  // Everything between the cover tile and the button hints: one row per entry
  // in slots_, then the four-icon nav strip, whose selected item's label sits
  // above the strip's top rule rather than under its own icon (a label as long
  // as "File Transfer" does not fit a quarter-width cell). Immediate-mode: it
  // draws once per render() and never schedules a repaint of its own, because
  // a repaint that lands mid-download has no heap to run in.
  void drawSlotBand(const SlotBandLayout& layout, int selectedSlot, int selectedNav) const;
  // Selector positions the cover tile owns, ahead of the book rows. The
  // condition mirrors buildMenuLabels()'s old "is Continue Reading folded into
  // the menu" one: with it folded in the tile is a single selector position,
  // otherwise it is one per displayed cover.
  int coverSelectionCount() const;
  // Book rows the band actually draws -- the selector's middle stretch.
  int slotRowCount() const;
  // Which book row the selector is on, or -1 when it is on the tile or the
  // nav strip.
  int slotSelectionIndex() const;
  // Which of the four nav icons the selector is on, or -1 when it is still on
  // the tile or a book row. The selector runs tile -> rows -> nav strip, so
  // the four icons are always the last four positions.
  int navSelectionIndex() const;
  // Runs the row's one action: an undownloaded or failed book is enqueued, a
  // local one is opened, and a book already in the queue does nothing.
  void activateSlot(const home_book_slots::Slot& slot);
  // Puts a refused enqueue on screen. Nothing was queued, so no counter moved
  // and no repaint is coming; without a popup the row would just sit there.
  void showEnqueueRefused(download_queue::EnqueueOutcome outcome);
  // Set by trySyncLibrary() when a sync changed the index, consumed by the
  // next loop() pass. It cannot rebuild in place: trySyncLibrary() runs on the
  // render task with the rendering mutex already held, and rebuildSlots()
  // takes the non-recursive RenderLock.
  bool slotsRebuildPending = false;
  // Refreshes the rows when the queue moved, and only then: a book entering
  // the queue, a download starting, and a download ending are the three
  // moments Home may repaint, because during a transfer the largest
  // contiguous heap block is a few KB and a repaint that cannot allocate
  // aborts the device. Costs two integer compares on every other pass.
  void pollDownloadQueue();
  // Last download_queue::pulse() this activity acted on, seeded in onEnter()
  // so entering mid-download does not rebuild for a change that predates it.
  uint32_t lastPulseGeneration = 0;
  uint32_t lastPulseCompletions = 0;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  // Refreshes tileBooks and recencyList from RECENT_BOOKS. Assumes the caller
  // holds RenderLock -- it is only ever reached through rebuildSlots(), which
  // takes one.
  void loadRecentBooks();
  void loadRecentCovers(int coverHeight);
  // Re-reads the recency list and stores home_book_slots::fill()'s result into
  // slots_, all under one RenderLock (see AGENTS.md's RenderLock rule;
  // render() reads both lists on the render task). Called at exactly the three
  // moments Home may repaint -- enqueue, download start, download end -- plus
  // onEnter(); the re-read is what makes a completed download's
  // markDownloaded() and a sync's discoveries visible here.
  void rebuildSlots();
  // Automatic "check whether there are new files" sync: runs at most once
  // per boot, the first time the library screen is reached, and only if
  // already paired (see lib/SyncManifest/SyncTriggerPolicy.h for the exact
  // rules). If WiFi is not already connected, this brings it up itself
  // first -- bounded, back-off shared with the before-sleep sync (see
  // src/sync/SleepProgressSync.h) -- before checking whether to run the
  // sync itself. Called from render(), right after the recent-covers
  // loading stage, on the same "blocking with a visible popup" pattern
  // loadRecentCovers() itself uses.
  void trySyncLibrary();
  // Merges what the sync just learned into the one recency list: books the
  // server has that this device does not are inserted at the front, and
  // remote entries whose book was deleted server-side are dropped (see
  // lib/RecentDiscovery, which owns the rule, and
  // docs/superpowers/specs/2026-09-02-one-recency-list-design.md). Called
  // only from trySyncLibrary(), only after a successful sync, and
  // deliberately touches RECENT_BOOKS rather than slots_: it runs on the
  // render task with the rendering mutex held, so the repaint it needs goes
  // through slotsRebuildPending like every other post-sync change.
  void runRecentDiscovery();
  // Delivers CrossPointState::pendingBookFinishedPath, if there is one and
  // conditions allow (see SyncTriggerPolicy.h's
  // shouldDeliverPendingBookFinished()) -- the reporting half of
  // ReaderActivity's book-finished detection, deferred to here for heap
  // headroom (see BookFinishedNotifier.h). Gated per-visit
  // (bookFinishedAttemptedThisVisit, a plain member -- reset on every fresh
  // HomeActivity, unlike trySyncLibrary()'s per-boot static), not per boot:
  // a small telemetry POST is cheap enough to retry on every distinct visit,
  // and doing so lets a second book finished later in the same boot still
  // get reported once the user leaves and returns to Home, without waiting
  // for the next reboot.
  void tryDeliverPendingBookFinished();
  bool bookFinishedAttemptedThisVisit = false;
  // Reconciles /.sleep against the wallpapers the server has assigned to this
  // reader (src/sync/WallpaperSync.h). Runs after trySyncLibrary(), never on
  // the same render pass as one of its popups, and -- unlike the library sync
  // -- not on every boot: the cadence is a persisted boot count, since the
  // board has no clock (see SyncTriggerPolicy.h's
  // WALLPAPER_SYNC_BOOT_INTERVAL). Never brings Wi-Fi up itself; it rides on
  // whatever the library sync's own bring-up left connected.
  void trySyncWallpapers();

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE, bool cleanInitialRefresh = false)
      : Activity("Home", renderer, mappedInput),
        initialMenuItem(initialMenuItemValue),
        cleanInitialRefresh(cleanInitialRefresh) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
#ifdef CP_TEST_CONSOLE
  bool getSelectedRowInfo(std::string& outLabel, int& outIndex, int& outCount) const override;
#endif
};
