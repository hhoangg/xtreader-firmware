#pragma once
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
  std::vector<RecentBook> recentBooks;
  const HomeMenuItem initialMenuItem;

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
  // The menu's text labels, in display order (File Browser/Recents/File
  // Transfer/Settings, with Continue Reading prepended when
  // the active theme's homeContinueReadingInMenu is set and there's a
  // recent book). Factored out of render() so CMD:SELECTED's accessor below
  // can report the same list without duplicating -- or drifting from --
  // render()'s conditional insert logic. Icons are still built inline in
  // render(); only the text labels are shared.
  std::vector<const char*> buildMenuLabels() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);
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
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItemValue) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
#ifdef CP_TEST_CONSOLE
  bool getSelectedRowInfo(std::string& outLabel, int& outIndex, int& outCount) const override;
#endif
};
