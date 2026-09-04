#include "XtreaderActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <memory>
#include <string>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "SyncCredentialStore.h"
#include "SyncPairingActivity.h"
#include "WallpaperGalleryActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "sync/DownloadQueue.h"
#include "sync/SleepProgressSync.h"
#include "sync/SyncManifest.h"
#include "sync/Telemetry.h"
#include "util/ButtonNavigator.h"
#include "util/StringUtils.h"

namespace fui = freeink::ui;

namespace {

// Flash-resident row tables. TAB_ROWS[tab][i] is the row id at position i of
// that tab; positions past TAB_ROW_COUNT[tab] are never read.
constexpr XtreaderActivity::RowId TAB_ROWS[XtreaderActivity::TAB_COUNT][XtreaderActivity::MAX_TAB_ROWS] = {
    {XtreaderActivity::ROW_SERVER_URL, XtreaderActivity::ROW_STATUS, XtreaderActivity::ROW_PAIR_ACTION,
     XtreaderActivity::ROW_COUNT, XtreaderActivity::ROW_COUNT},
    {XtreaderActivity::ROW_REQUEST_BOOKS, XtreaderActivity::ROW_QUEUE, XtreaderActivity::ROW_SYNC_NOW,
     XtreaderActivity::ROW_DOC_MATCHING, XtreaderActivity::ROW_SEND_METADATA},
    {XtreaderActivity::ROW_WALLPAPER_GALLERY, XtreaderActivity::ROW_SYNCED_ONLY, XtreaderActivity::ROW_COUNT,
     XtreaderActivity::ROW_COUNT, XtreaderActivity::ROW_COUNT},
};

constexpr int TAB_ROW_COUNT[XtreaderActivity::TAB_COUNT] = {3, 5, 2};

// Compiler-checks what the array declaration above can't: every entry is a
// count that indexes rowItems_/rowValues_, both sized MAX_TAB_ROWS. A count
// outside (0, MAX_TAB_ROWS] compiles cleanly as an int but writes one past the
// end of those fixed member arrays at runtime.
constexpr bool allTabRowCountsInRange() {
  for (int count : TAB_ROW_COUNT) {
    if (count <= 0 || count > XtreaderActivity::MAX_TAB_ROWS) return false;
  }
  return true;
}
static_assert(allTabRowCountsInRange(), "TAB_ROW_COUNT entries must be in (0, MAX_TAB_ROWS]");

constexpr StrId TAB_LABELS[XtreaderActivity::TAB_COUNT] = {StrId::STR_CAT_ACCOUNT, StrId::STR_CAT_LIBRARY,
                                                           StrId::STR_CAT_WALLPAPERS};

// Character cap for the Server URL row's *value*. The list row widget draws
// item.value right-aligned and never truncates it, so an over-long value
// takes width from the label until the label disappears off the row (the
// label's Rect goes to zero/negative width and GfxRendererTarget::text()
// draws nothing for it).
//
// Measured with tools/render_ui_screens against the real UI fonts and Lyra
// theme geometry (the default theme, CrossPointSettings.h's `uiTheme =
// LYRA`): on the X4's 480px-wide portrait band, the row content is 424px
// (listInset=20, listSidePadding=8 either side); "Server URL" sets in Ubuntu
// 12pt regular (the row's label font) at 125px; the value (Ubuntu 10pt
// regular) has an 8px valueInset and 10px textGap before it, leaving a
// 281px budget. 30 is the exact-fit maximum that
// StringUtils::middleEllipsis() can keep within that budget alongside the
// complete label -- 34 already clips it. That was measured against the
// 41-character workers.dev hostname this firmware used to default to; the
// default is now https://xtreader.com at 20 characters and never truncates
// at all, so what the cap protects is a self-hosted URL, which can be any
// length. middleEllipsis() is a
// character-count heuristic, not a pixel measurement (see its own doc
// comment), so a self-hosted hostname at the same character count but wider
// glyphs (more digits/caps) could still clip the label at exactly 30 -- this
// row has already been fixed three times on exactly that kind of margin
// error, so it stays at 28: two characters of headroom, invisible on any
// hostname anyone actually reads in full, in exchange for the failure mode
// not existing.
constexpr size_t SERVER_URL_VALUE_MAX_CHARS = 28;

}  // namespace

XtreaderActivity::XtreaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiTabListActivity("Xtreader", renderer, mappedInput) {}

void XtreaderActivity::onEnter() {
  UiTabListActivity::onEnter();
  selectTab(0);
}

const char* XtreaderActivity::tabLabel(const int index) const {
  if (index < 0 || index >= TAB_COUNT) return "";
  return I18N.get(TAB_LABELS[index]);
}

int XtreaderActivity::listCount() const { return TAB_ROW_COUNT[activeTab_]; }

XtreaderActivity::RowId XtreaderActivity::rowIdAt(const int position) const {
  if (position < 0 || position >= TAB_ROW_COUNT[activeTab_]) return ROW_COUNT;
  return TAB_ROWS[activeTab_][position];
}

void XtreaderActivity::selectTab(const int index) {
  activeTab_ = index;
  activeNav().top = 0;  // a tab switch starts its list at the top
  rebuildRowItems();
}

// Labels that never change are set here, once per tab switch. The three rows
// whose label itself depends on live state (pair/unlink, queue/cancel) are
// refreshed in buildScreen() instead.
void XtreaderActivity::rebuildRowItems() {
  const int count = TAB_ROW_COUNT[activeTab_];
  for (int i = 0; i < MAX_TAB_ROWS; i++) {
    rowItems_[i] = fui::ListItem{};
    rowValues_[i].clear();
  }
  for (int i = 0; i < count; i++) {
    rowItems_[i].actionValue = static_cast<int16_t>(i);
    switch (TAB_ROWS[activeTab_][i]) {
      case ROW_SERVER_URL:
        rowItems_[i].label = tr(STR_CROSSPOINT_SYNC_SERVER_URL);
        break;
      case ROW_STATUS:
        rowItems_[i].label = tr(STR_PAIRING_STATUS);
        break;
      case ROW_REQUEST_BOOKS:
        rowItems_[i].label = tr(STR_REQUEST_BOOKS);
        break;
      case ROW_SYNC_NOW:
        rowItems_[i].label = tr(STR_SYNC_NOW);
        break;
      case ROW_DOC_MATCHING:
        rowItems_[i].label = tr(STR_DOCUMENT_MATCHING);
        break;
      case ROW_SEND_METADATA:
        rowItems_[i].label = tr(STR_SEND_METADATA);
        break;
      case ROW_WALLPAPER_GALLERY:
        rowItems_[i].label = tr(STR_WALLPAPER_GALLERY);
        break;
      case ROW_SYNCED_ONLY:
        rowItems_[i].label = tr(STR_SLEEP_SYNCED_ONLY);
        break;
      default:
        // ROW_PAIR_ACTION and ROW_QUEUE: label set per repaint in buildScreen().
        break;
    }
  }
}

void XtreaderActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});

  buildTabBar(screen);

  const int count = TAB_ROW_COUNT[activeTab_];
  const bool paired = SYNC_STORE.isPaired();

  for (int i = 0; i < count; i++) {
    switch (TAB_ROWS[activeTab_][i]) {
      case ROW_SERVER_URL: {
        // Just the URL -- no "Default: " prefix. The row is already labelled
        // "Server URL"; the prefix told the reader nothing they couldn't
        // already see, and every character it spent was one the label needed.
        std::string url = SYNC_STORE.getServerUrl();
        if (url.empty()) {
          url = SYNC_STORE.getBaseUrl();
          const auto schemeEnd = url.find("://");
          if (schemeEnd != std::string::npos) url.erase(0, schemeEnd + 3);
        }
        rowValues_[i] = StringUtils::middleEllipsis(url, SERVER_URL_VALUE_MAX_CHARS);
        break;
      }
      case ROW_STATUS:
        rowValues_[i] = paired ? SYNC_STORE.getAccountEmail() : tr(STR_NOT_PAIRED);
        break;
      case ROW_PAIR_ACTION:
        rowItems_[i].label = paired ? tr(STR_UNLINK_DEVICE) : tr(STR_PAIR_DEVICE);
        rowValues_[i] = paired ? SYNC_STORE.getDeviceName() : "";
        break;
      case ROW_REQUEST_BOOKS:
        rowValues_[i] = requestBooksStatus_;
        break;
      case ROW_QUEUE: {
        const download_queue::Snapshot queueSnap = download_queue::snapshot();
        rowItems_[i].label = queueSnap.count > 0 ? tr(STR_CANCEL_DOWNLOADS) : tr(STR_DOWNLOAD_QUEUE);
        rowValues_[i] = queueSnap.count > 0 ? std::to_string(queueSnap.count) : "";
        break;
      }
      case ROW_SYNC_NOW:
        rowValues_[i] = syncNowStatus_;
        break;
      case ROW_DOC_MATCHING:
        rowValues_[i] =
            KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::BINARY ? tr(STR_BINARY) : tr(STR_FILENAME);
        break;
      case ROW_SEND_METADATA:
        rowValues_[i] = KOREADER_STORE.getSendMetadata() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        break;
      case ROW_SYNCED_ONLY:
        rowValues_[i] = SETTINGS.sleepScreenSyncedOnly ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        break;
      default:
        break;
    }
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  // Titles match the value's font size so both sides of a row read as one unit;
  // labels that still don't fit wrap onto a second line. maxLines also marks
  // the style explicitly set, so the list does not substitute bodyText back.
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncTabListViewport(screen, props);
  screen.list(props);
}

void XtreaderActivity::onTabAction(const int index) {
  selectTab(index);
  activeNav().selected = 0;  // tab taps land with the tab bar focused
  // The switched-to tab repaints as the selected pill; a flash overlay on top
  // of it would just repaint the pill in the focused style.
  app.clearTapFlash();
}

void XtreaderActivity::stepTab(const int direction) {
  // Ring position 0 stays on the tab bar; a row selection collapses to the new
  // tab's first row (per-tab memory is deliberately not kept here).
  const bool onTabBar = ringPos() == 0;
  const int next = direction > 0 ? ButtonNavigator::nextIndex(activeTab_, TAB_COUNT)
                                 : ButtonNavigator::previousIndex(activeTab_, TAB_COUNT);
  selectTab(next);
  activeNav().selected = onTabBar ? 0 : 1;
  requestUpdate();
}

bool XtreaderActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ringPos() == 0) {
      stepTab(1);
    } else {
      // activateRow(), not activateIndex(): the latter's tap-first focus reset
      // belongs to the touch path only.
      activateRow(ringPos() - 1);
      requestUpdate();
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (ringPos() > 0) {
      activeNav().selected = 0;
      requestUpdate();
    } else {
      SETTINGS.saveToFile();
      onGoHome();
    }
    return true;
  }

  return false;
}

void XtreaderActivity::drawFooter() {
  const int ring = ringPos();
  const char* confirmLabel = tr(STR_SELECT);
  if (ring == 0) {
    confirmLabel = I18N.get(TAB_LABELS[(activeTab_ + 1) % TAB_COUNT]);
  } else {
    switch (rowIdAt(ring - 1)) {
      case ROW_DOC_MATCHING:
      case ROW_SEND_METADATA:
      case ROW_SYNCED_ONLY:
        confirmLabel = tr(STR_TOGGLE);
        break;
      default:
        break;
    }
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void XtreaderActivity::activateIndex(const int index) {
  activateRow(index);

  // Tap-first: a tapped row is not a cursor position. Leaving it focused
  // (inverted) after the tap meant the row stayed black once its sub-screen or
  // popup closed, and Back then had to clear that focus before a second Back
  // left the screen. Hand focus back to the tab band; the viewport stays put.
  if (mappedInput.hasTouch()) {
    activeNav().selected = 0;
  }
}

void XtreaderActivity::activateRow(const int position) {
  switch (rowIdAt(position)) {
    case ROW_SERVER_URL:
      editServerUrl();
      break;
    case ROW_PAIR_ACTION:
      if (SYNC_STORE.isPaired()) {
        unlinkDevice();
      } else {
        launchPairing();
      }
      break;
    case ROW_REQUEST_BOOKS:
      requestBooksTapped();
      break;
    case ROW_QUEUE:
      toggleQueueCancel();
      break;
    case ROW_SYNC_NOW:
      syncNowTapped();
      break;
    case ROW_DOC_MATCHING:
      cycleDocumentMatching();
      break;
    case ROW_SEND_METADATA:
      cycleSendMetadata();
      break;
    case ROW_WALLPAPER_GALLERY:
      openWallpaperGallery();
      break;
    case ROW_SYNCED_ONLY:
      toggleSyncedWallpapersOnly();
      break;
    default:
      // ROW_STATUS is informational only -- no action.
      break;
  }
}

void XtreaderActivity::openWallpaperGallery() {
  app.clearTapFlash();
  startActivityForResult(std::make_unique<WallpaperGalleryActivity>(renderer, mappedInput),
                         [this](const ActivityResult&) { requestUpdate(); });
}

// Two-value enums, so Confirm cycles rather than opening a picker. The
// setters only assign the in-memory field, so the explicit saveToFile() below
// is what persists the change -- drop it and the setting would silently
// revert on the next reboot.
void XtreaderActivity::cycleDocumentMatching() {
  app.clearTapFlash();
  const bool wasBinary = KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::BINARY;
  KOREADER_STORE.setMatchMethod(wasBinary ? DocumentMatchMethod::FILENAME : DocumentMatchMethod::BINARY);
  KOREADER_STORE.saveToFile();
  requestUpdate();
}

void XtreaderActivity::cycleSendMetadata() {
  app.clearTapFlash();
  KOREADER_STORE.setSendMetadata(!KOREADER_STORE.getSendMetadata());
  KOREADER_STORE.saveToFile();
  requestUpdate();
}

void XtreaderActivity::toggleSyncedWallpapersOnly() {
  app.clearTapFlash();
  SETTINGS.sleepScreenSyncedOnly = SETTINGS.sleepScreenSyncedOnly ? 0 : 1;
  SETTINGS.saveToFile();
  requestUpdate();
}

#ifdef CP_TEST_CONSOLE
bool XtreaderActivity::getSelectedRowInfo(std::string& outLabel, int& outIndex, int& outCount) const {
  const int ring = ringPos();
  outIndex = ring;
  outCount = TAB_ROW_COUNT[activeTab_] + 1;  // +1 for the tab band at ring 0
  if (ring == 0) {
    // Tab band focused: report the active tab's own label. CONFIRM here cycles
    // tabs, it does not activate a row -- callers must NAVNEXT off the tab band
    // before searching rows.
    outLabel = tabLabel(activeTab_);
    return true;
  }
  const int row = ring - 1;
  if (row < 0 || row >= TAB_ROW_COUNT[activeTab_]) {
    outLabel.clear();
    return true;
  }
  outLabel = rowItems_[row].label ? rowItems_[row].label : "";
  return true;
}
#endif

void XtreaderActivity::editServerUrl() {
  app.clearTapFlash();
  const std::string currentUrl = SYNC_STORE.getServerUrl();
  const std::string prefillUrl = currentUrl.empty() ? "https://" : currentUrl;
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_CROSSPOINT_SYNC_SERVER_URL), prefillUrl,
                                              128, InputType::Url),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          const auto& kb = std::get<KeyboardResult>(result.data);
          const std::string urlToSave = (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
          SYNC_STORE.setServerUrl(urlToSave);
        }
      });
}

void XtreaderActivity::launchPairing() {
  app.clearTapFlash();
  startActivityForResult(std::make_unique<SyncPairingActivity>(renderer, mappedInput),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void XtreaderActivity::unlinkDevice() {
  app.clearTapFlash();
  SYNC_STORE.clearPairing();
  // The provisioned progress-sync credential is only valid for this device's
  // (now-forgotten) pairing -- leaving it configured would keep progress
  // sync silently working against an account this device is no longer
  // linked to. See KOReaderCredentialStore's provisioned-credential comment.
  KOREADER_STORE.clearProvisionedCredential();
  requestUpdate();
}

void XtreaderActivity::doRequestBooks() {
  const telemetry::TelemetryResult result = telemetry::requestBooks();
  requestBooksStatus_ = result.ok ? tr(STR_REQUEST_BOOKS_SENT) : tr(STR_REQUEST_BOOKS_FAILED);
  requestUpdate();
}

// POST /feedback/request-books -- the product's whole "I want more books"
// button (docs/API.md's "Feedback and telemetry"). Brings WiFi up first,
// same launch-WifiSelectionActivity-then-proceed pattern SyncPairingActivity
// uses for its own first network call.
void XtreaderActivity::requestBooksTapped() {
  app.clearTapFlash();
  if (WiFi.status() != WL_CONNECTED) {
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               requestBooksStatus_ = tr(STR_REQUEST_BOOKS_FAILED);
                               requestUpdate();
                             } else {
                               doRequestBooks();
                             }
                           });
    return;
  }
  doRequestBooks();
}

// The Download Queue row doubles as its own cancel action: tapping it while
// something is queued/downloading empties the queue (download_queue's
// cancelAll()) -- "someone who queued ten books by mistake needs a way out
// that is not a reboot," per the task brief. A tap while the queue is
// already empty is a no-op.
void XtreaderActivity::toggleQueueCancel() {
  app.clearTapFlash();
  if (download_queue::snapshot().count == 0) return;
  download_queue::cancelAll();
  requestUpdate();
}

// sync_manifest::sync() -- the explicit counterpart to HomeActivity's
// automatic once-per-boot trigger (SyncTriggerPolicy.h). Same
// visible-while-it-happens popup HomeActivity's own trySyncLibrary() uses.
void XtreaderActivity::doManifestSync() {
  {
    // This runs on the loop task (an activateIndex()/activity-result-handler
    // call), not the render task, so drawing directly needs the same
    // RenderLock FileBrowserActivity's own force-delete popups take for the
    // same reason.
    RenderLock lock(*this);
    GUI.drawPopup(renderer, tr(STR_SYNCING_LIBRARY));
  }
  const sync_manifest::SyncResult result = sync_manifest::sync();
  syncNowStatus_ = result.ok ? tr(STR_SYNC_NOW_DONE) : tr(STR_SYNC_NOW_FAILED);

  // A completed manifest fetch proves this location has working Wi-Fi, so it
  // clears the shared sleep/library back-off (SleepProgressSync.h): without
  // this, a device that escalated its skip count while away kept refusing the
  // automatic before-sleep upload for several more power-offs after the owner
  // came home and watched this very button succeed.
  if (result.ok) sleep_progress_sync::noteNetworkReached();

  // The radio is already up and paid for by the sync above, so the heartbeat
  // rides along -- same arrangement HomeActivity::trySyncLibrary() uses. This
  // is the only button that forces a refresh on demand, so leaving it out
  // meant the one deliberate way to update the device's own figures did not.
  // Best-effort: diagnostics must not change what the reader is told about
  // their library sync.
  telemetry::HeartbeatInfo heartbeatInfo = telemetry::currentDeviceHeartbeatInfo();
  heartbeatInfo.lastSyncStatus = result.ok ? "ok" : "failed";
  const telemetry::TelemetryResult heartbeatResult = telemetry::sendHeartbeat(heartbeatInfo);
  if (!heartbeatResult.ok) {
    LOG_DBG("XTRA", "Heartbeat piggybacked on Sync Now failed (error=%s status=%d) -- diagnostics only",
            heartbeatResult.error.c_str(), heartbeatResult.httpStatus);
  }

  requestUpdate();
}

// Same "bring WiFi up first if needed" pattern as requestBooksTapped() above
// -- an explicit, deliberate tap, not the automatic background trigger, so
// bringing the radio up here is expected rather than a surprise battery cost.
void XtreaderActivity::syncNowTapped() {
  app.clearTapFlash();
  if (WiFi.status() != WL_CONNECTED) {
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               syncNowStatus_ = tr(STR_SYNC_NOW_FAILED);
                               requestUpdate();
                             } else {
                               doManifestSync();
                             }
                           });
    return;
  }
  doManifestSync();
}
