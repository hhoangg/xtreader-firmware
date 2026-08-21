#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <SyncTriggerPolicy.h>
#include <Utf8.h>
#include <WiFi.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "SleepWifiBackoffPolicy.h"
#include "SyncCredentialStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "sync/BookFinishedNotifier.h"
#include "sync/SleepProgressSync.h"
#include "sync/SyncManifest.h"
#include "sync/Telemetry.h"

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
}  // namespace

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Recents, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  return count;
}

#ifdef CP_TEST_CONSOLE
bool HomeActivity::getSelectedRowInfo(std::string& outLabel, int& outIndex, int& outCount) const {
  outIndex = selectorIndex;
  outCount = getMenuItemCount();

  const auto menuItems = buildMenuLabels();
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Mirrors render()'s selectedIndex math for GUI.drawButtonMenu(): when
  // Continue Reading is folded into the menu, selectorIndex indexes menuItems
  // directly; otherwise the first recentBooks.size() values of selectorIndex
  // pick a recent-book cover tile instead (no text label -- see below).
  const int menuIndex =
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - static_cast<int>(recentBooks.size());

  if (menuIndex < 0 || menuIndex >= static_cast<int>(menuItems.size())) {
    outLabel.clear();  // selection is on a recent-book cover tile, which has no text label
    return true;
  }
  outLabel = menuItems[menuIndex];
  return true;
}
#endif

std::vector<const char*> HomeActivity::buildMenuLabels() const {
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  if (UITheme::getInstance().getMetrics().homeContinueReadingInMenu && !recentBooks.empty()) {
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
  }
  return menuItems;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
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
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
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
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
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

void HomeActivity::onEnter() {
  Activity::onEnter();

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem);

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

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto activateSelection = [this] {
    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
    const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
    switch (indexToMenuItem(menuIndex)) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::RECENTS:
        onRecentsOpen();
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

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }

  // Back is otherwise unused on the home menu: open the most recently read
  // book directly (recentBooks is most-recent-first and already pruned of
  // files missing from the SD card).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && !recentBooks.empty()) {
    onSelectBook(recentBooks[0].path);
    return;
  }

  const int coverColumnCount = std::max(1, metrics.homeRecentBooksCount);
  const int recentCount = std::min(static_cast<int>(recentBooks.size()), coverColumnCount);
  const int coverColumnWidth = (renderer.getScreenWidth() - 2 * metrics.contentSidePadding) / coverColumnCount;
  int touchedBook = -1;
  const auto coverTouch = mappedInput.colTouch(touchedBook, metrics.contentSidePadding, coverColumnWidth, recentCount,
                                               metrics.homeTopPadding,
                                               metrics.homeTopPadding + metrics.homeCoverTileHeight, coverColumnWidth);
  if (coverTouch != MappedInputManager::RowTouch::None) {
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

  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int renderedMenuSelection =
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size();
  const int renderedMenuCount =
      menuCount - (metrics.homeContinueReadingInMenu ? 0 : static_cast<int>(recentBooks.size()));
  int menuRow = -1;
  // Row height from the theme, not the metrics table: RoundedRaff draws
  // font-derived rows and the touch grid must match the visuals exactly.
  const int menuRowHeight = GUI.getMenuRowHeight(renderer);
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuTop, menuRowHeight + metrics.menuSpacing, renderedMenuCount,
                                              0, INT32_MAX, menuRowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex =
        metrics.homeContinueReadingInMenu ? menuRow : menuRow + static_cast<int>(recentBooks.size());
    if (menuTouch == MappedInputManager::RowTouch::Down) {
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
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  // Band spans topPadding..homeTopPadding: the cover tile starts at the fixed
  // homeTopPadding, so the height must shrink by topPadding or the band (and a
  // centered title, e.g. RoundedRaff's book title) sinks into the tile.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding - metrics.topPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = 0;
  coverRectY = metrics.homeTopPadding;
  coverRectW = pageWidth;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  // Build menu items dynamically. Labels come from buildMenuLabels() (shared
  // with CMD:SELECTED's introspection, see HomeActivity.h); icons are built
  // here with the exact same conditions since CMD:SELECTED has no use for them.
  std::vector<const char*> menuItems = buildMenuLabels();
  std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings};

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuIcons.insert(menuIcons.begin(), Book);
  }

  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                         metrics.homeMenuTopOffset + metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

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
    trySyncLibrary();
    tryDeliverPendingBookFinished();
  }
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
      GUI.drawPopup(renderer, tr(STR_CONNECTING_SAVED_WIFI));
      bool cancelled = false;
      // This runs on the render task, inside HomeActivity::render(), which
      // already holds ActivityManager's rendering mutex for the whole call
      // (see ActivityManager::renderTaskLoop()) -- connectToSavedWifi() must
      // not try to take it again itself, or the render task deadlocks
      // against itself (renderingMutex is not recursive). See that
      // function's header comment.
      wifiConnected = sleep_progress_sync::connectToSavedWifi(cancelled, /*callerHoldsRenderLock=*/true);
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
  GUI.drawPopup(renderer, tr(STR_SYNCING_LIBRARY));
  // Bounded, but with its own budget rather than the shorter power-off one --
  // see SyncTriggerPolicy.h's HOME_SYNC_TIMEOUT_MS for why the two differ.
  // A captive portal or black-holed server still must not stall the render
  // task behind the popup above indefinitely.
  const sync_manifest::SyncResult syncResult = sync_manifest::sync(sync_trigger::HOME_SYNC_TIMEOUT_MS);
  // FileBrowserActivity reads whatever landed on SD; syncResult itself is only used below.
  requestUpdate();  // redraw Home without the popup

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

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }
