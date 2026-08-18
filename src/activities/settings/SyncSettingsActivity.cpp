#include "SyncSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <memory>
#include <string>

#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "SyncCredentialStore.h"
#include "SyncPairingActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "sync/DownloadQueue.h"
#include "sync/SleepProgressSync.h"
#include "sync/SyncManifest.h"
#include "sync/Telemetry.h"
#include "util/StringUtils.h"

namespace fui = freeink::ui;

namespace {
enum RowIndex : int {
  ROW_SERVER_URL = 0,
  ROW_STATUS = 1,
  ROW_PAIR_ACTION = 2,
  ROW_REQUEST_BOOKS = 3,
  ROW_QUEUE = 4,
  ROW_SYNC_NOW = 5,
};

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
// 281px budget. 30 is the exact-fit maximum for the production default
// (crosspoint-sync.hoangxuan2402.workers.dev, 41 chars) that
// StringUtils::middleEllipsis() can keep within that budget alongside the
// complete label -- 34 already clips it. middleEllipsis() is a
// character-count heuristic, not a pixel measurement (see its own doc
// comment), so a self-hosted hostname at the same character count but wider
// glyphs (more digits/caps) could still clip the label at exactly 30 -- this
// row has already been fixed three times on exactly that kind of margin
// error, so it stays at 28: two characters of headroom, invisible on any
// hostname anyone actually reads in full, in exchange for the failure mode
// not existing.
constexpr size_t SERVER_URL_VALUE_MAX_CHARS = 28;
}  // namespace

SyncSettingsActivity::SyncSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("SyncSettings", renderer, mappedInput) {
  rowItems_[ROW_SERVER_URL].label = tr(STR_CROSSPOINT_SYNC_SERVER_URL);
  rowItems_[ROW_SERVER_URL].actionValue = ROW_SERVER_URL;
  rowItems_[ROW_STATUS].label = tr(STR_PAIRING_STATUS);
  rowItems_[ROW_STATUS].actionValue = ROW_STATUS;
  // Label refreshed per buildScreen() call -- it toggles with pairing state.
  rowItems_[ROW_PAIR_ACTION].actionValue = ROW_PAIR_ACTION;
  rowItems_[ROW_REQUEST_BOOKS].label = tr(STR_REQUEST_BOOKS);
  rowItems_[ROW_REQUEST_BOOKS].actionValue = ROW_REQUEST_BOOKS;
  // Label refreshed per buildScreen() call -- it toggles with queue state,
  // same as ROW_PAIR_ACTION above.
  rowItems_[ROW_QUEUE].actionValue = ROW_QUEUE;
  rowItems_[ROW_SYNC_NOW].label = tr(STR_SYNC_NOW);
  rowItems_[ROW_SYNC_NOW].actionValue = ROW_SYNC_NOW;
}

int SyncSettingsActivity::listCount() const { return MENU_ITEMS; }

const char* SyncSettingsActivity::headerTitle() const { return tr(STR_ACCOUNT_SYNC); }

void SyncSettingsActivity::launchPairing() {
  app.clearTapFlash();
  startActivityForResult(std::make_unique<SyncPairingActivity>(renderer, mappedInput),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void SyncSettingsActivity::unlinkDevice() {
  app.clearTapFlash();
  SYNC_STORE.clearPairing();
  // The provisioned progress-sync credential is only valid for this device's
  // (now-forgotten) pairing -- leaving it configured would keep progress
  // sync silently working against an account this device is no longer
  // linked to. See KOReaderCredentialStore's provisioned-credential comment.
  KOREADER_STORE.clearProvisionedCredential();
  requestUpdate();
}

void SyncSettingsActivity::doRequestBooks() {
  const telemetry::TelemetryResult result = telemetry::requestBooks();
  requestBooksStatus_ = result.ok ? tr(STR_REQUEST_BOOKS_SENT) : tr(STR_REQUEST_BOOKS_FAILED);
  requestUpdate();
}

// POST /feedback/request-books -- the product's whole "I want more books"
// button (docs/API.md's "Feedback and telemetry"). Brings WiFi up first,
// same launch-WifiSelectionActivity-then-proceed pattern SyncPairingActivity
// uses for its own first network call.
void SyncSettingsActivity::requestBooksTapped() {
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
void SyncSettingsActivity::toggleQueueCancel() {
  app.clearTapFlash();
  if (download_queue::snapshot().count == 0) return;
  download_queue::cancelAll();
  requestUpdate();
}

// sync_manifest::sync() -- the explicit counterpart to HomeActivity's
// automatic once-per-boot trigger (SyncTriggerPolicy.h). Same
// visible-while-it-happens popup HomeActivity's own trySyncLibrary() uses.
void SyncSettingsActivity::doManifestSync() {
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
    LOG_DBG("SYNCSET", "Heartbeat piggybacked on Sync Now failed (error=%s status=%d) -- diagnostics only",
            heartbeatResult.error.c_str(), heartbeatResult.httpStatus);
  }

  requestUpdate();
}

// Same "bring WiFi up first if needed" pattern as requestBooksTapped() above
// -- an explicit, deliberate tap, not the automatic background trigger, so
// bringing the radio up here is expected rather than a surprise battery cost.
void SyncSettingsActivity::syncNowTapped() {
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

void SyncSettingsActivity::activateIndex(const int index) {
  if (index == ROW_SERVER_URL) {
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
  } else if (index == ROW_PAIR_ACTION) {
    if (SYNC_STORE.isPaired()) {
      unlinkDevice();
    } else {
      launchPairing();
    }
  } else if (index == ROW_REQUEST_BOOKS) {
    requestBooksTapped();
  } else if (index == ROW_QUEUE) {
    toggleQueueCancel();
  } else if (index == ROW_SYNC_NOW) {
    syncNowTapped();
  }
  // ROW_STATUS is informational only -- no action.
}

#ifdef CP_TEST_CONSOLE
bool SyncSettingsActivity::getSelectedRowInfo(std::string& outLabel, int& outIndex, int& outCount) const {
  outIndex = nav.selected;
  outCount = MENU_ITEMS;
  if (nav.selected < 0 || nav.selected >= MENU_ITEMS) {
    outLabel.clear();
    return true;
  }
  outLabel = rowItems_[nav.selected].label ? rowItems_[nav.selected].label : "";
  return true;
}
#endif

void SyncSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const bool paired = SYNC_STORE.isPaired();

  // Just the URL -- no "Default: " prefix. The row is already labelled
  // "Server URL"; the prefix told the reader nothing they couldn't already
  // see, and every character it spent was a character the label needed.
  std::string url = SYNC_STORE.getServerUrl();
  if (url.empty()) {
    url = SYNC_STORE.getBaseUrl();
    const auto schemeEnd = url.find("://");
    if (schemeEnd != std::string::npos) url.erase(0, schemeEnd + 3);
  }
  rowValues_[ROW_SERVER_URL] = StringUtils::middleEllipsis(url, SERVER_URL_VALUE_MAX_CHARS);

  rowValues_[ROW_STATUS] = paired ? SYNC_STORE.getAccountEmail() : tr(STR_NOT_PAIRED);

  rowItems_[ROW_PAIR_ACTION].label = paired ? tr(STR_UNLINK_DEVICE) : tr(STR_PAIR_DEVICE);
  rowValues_[ROW_PAIR_ACTION] = paired ? SYNC_STORE.getDeviceName() : "";

  rowValues_[ROW_REQUEST_BOOKS] = requestBooksStatus_;

  const download_queue::Snapshot queueSnap = download_queue::snapshot();
  rowItems_[ROW_QUEUE].label = queueSnap.count > 0 ? tr(STR_CANCEL_DOWNLOADS) : tr(STR_DOWNLOAD_QUEUE);
  rowValues_[ROW_QUEUE] = queueSnap.count > 0 ? std::to_string(queueSnap.count) : "";

  rowValues_[ROW_SYNC_NOW] = syncNowStatus_;

  for (int i = 0; i < MENU_ITEMS; i++) {
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(MENU_ITEMS);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  syncListViewport(screen, props);
  screen.list(props);
}
