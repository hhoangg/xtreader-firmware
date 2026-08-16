#include "SyncSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <memory>
#include <string>

#include "MappedInputManager.h"
#include "SyncCredentialStore.h"
#include "SyncPairingActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "util/StringUtils.h"

namespace fui = freeink::ui;

namespace {
enum RowIndex : int { ROW_SERVER_URL = 0, ROW_STATUS = 1, ROW_PAIR_ACTION = 2 };

// Character cap for the Server URL row's *value*. The list row widget draws
// item.value right-aligned and never truncates it, so an over-long value
// takes width from the label until the label disappears off the row. The cap
// is a deliberately wide margin rather than a measured fit: the label must
// always render whole, and an abbreviated URL beside it is the acceptable
// trade. middleEllipsis() is a no-op once the value already fits, so a short
// self-hosted URL is left alone.
constexpr size_t SERVER_URL_VALUE_MAX_CHARS = 12;
}  // namespace

SyncSettingsActivity::SyncSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("SyncSettings", renderer, mappedInput) {
  rowItems_[ROW_SERVER_URL].label = tr(STR_CROSSPOINT_SYNC_SERVER_URL);
  rowItems_[ROW_SERVER_URL].actionValue = ROW_SERVER_URL;
  rowItems_[ROW_STATUS].label = tr(STR_PAIRING_STATUS);
  rowItems_[ROW_STATUS].actionValue = ROW_STATUS;
  // Label refreshed per buildScreen() call -- it toggles with pairing state.
  rowItems_[ROW_PAIR_ACTION].actionValue = ROW_PAIR_ACTION;
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
  requestUpdate();
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
