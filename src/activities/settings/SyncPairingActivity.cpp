#include "SyncPairingActivity.h"

#include <ArduinoJson.h>
#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <memory>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "SyncCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/QrUtils.h"

// One flat sentence + a short code per the pairing brief -- never a stack
// trace; the person holding this device cannot debug it and must be told to
// ask whoever set the account up. Member functions (not free functions in an
// anonymous namespace) because FailureReason is a private nested type.
const char* SyncPairingActivity::failureShortCode(FailureReason reason) {
  switch (reason) {
    case FailureReason::WIFI:
      return "P-01";
    case FailureReason::REQUEST_CODE:
      return "P-02";
    case FailureReason::DENIED:
      return "P-03";
    case FailureReason::EXPIRED:
      return "P-04";
    case FailureReason::POLL_ERROR:
      return "P-05";
  }
  return "P-00";
}

const char* SyncPairingActivity::failureMessage(FailureReason reason) {
  switch (reason) {
    case FailureReason::WIFI:
      return tr(STR_PAIRING_WIFI_FAILED_MSG);
    case FailureReason::REQUEST_CODE:
      return tr(STR_PAIRING_REQUEST_FAILED_MSG);
    case FailureReason::DENIED:
      return tr(STR_PAIRING_DENIED_MSG);
    case FailureReason::EXPIRED:
      return tr(STR_PAIRING_EXPIRED_MSG);
    case FailureReason::POLL_ERROR:
      return tr(STR_PAIRING_GENERIC_ERROR_MSG);
  }
  return tr(STR_PAIRING_GENERIC_ERROR_MSG);
}

SyncPairingActivity::SyncPairingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("SyncPairing", renderer, mappedInput) {}

void SyncPairingActivity::onEnter() {
  Activity::onEnter();
  screen_ = Screen::CONNECTING_WIFI;
  poller_ = DevicePairingPoller{};
  code_ = DeviceCodeResponse{};
  accountEmail_.clear();
  pairedDeviceName_.clear();
  lastShownMinutesRemaining_ = -1;
  didNetworkWork_ = false;
  startWifi();
}

void SyncPairingActivity::onExit() {
  Activity::onExit();
  // Matches KOReaderAuthActivity: a TLS session fragments the heap, so
  // reboot silently rather than leave that behind for whatever screen comes
  // next. Only worth doing if a request was actually attempted.
  if (didNetworkWork_ && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

bool SyncPairingActivity::preventAutoSleep() {
  // Only while a blocking network call could be in flight. The QR screen
  // deliberately does NOT prevent sleep: e-ink retains the QR after the
  // device sleeps (see the pairing brief), so a normal auto-sleep mid-wait
  // is fine and saves battery; polling simply resumes on the next loop().
  return screen_ == Screen::CONNECTING_WIFI || screen_ == Screen::REQUESTING_CODE;
}

void SyncPairingActivity::startWifi() {
  screen_ = Screen::CONNECTING_WIFI;
  if (WiFi.status() == WL_CONNECTED) {
    onWifiResult(true);
    return;
  }
  // autoConnect = true (the default): reuse WifiSelectionActivity's own
  // saved-network auto-connect exactly as KOReaderAuthActivity does: try the
  // last-known network, then any other saved network by signal strength,
  // falling back to its own picker only if none of that works. This screen
  // does not build a custom network picker.
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiResult(!result.isCancelled); });
}

void SyncPairingActivity::onWifiResult(const bool connected) {
  if (!connected) {
    fail(FailureReason::WIFI);
    return;
  }
  screen_ = Screen::REQUESTING_CODE;
  requestUpdate();
  requestCode();
}

void SyncPairingActivity::requestCode() {
  JsonDocument doc;
  doc["deviceLabel"] = BoardConfig::ACTIVE.name;
  std::string body;
  serializeJson(doc, body);

  const std::string url = SYNC_STORE.getBaseUrl() + "/device/code";
  LOG_DBG("SYNC", "Requesting pairing code: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());

  std::string response;
  int status = -1;
  didNetworkWork_ = true;
  const bool ok = HttpDownloader::postJson(url, body, response, &status);

  if (!ok || status != 201 || !parseDeviceCodeResponse(response.c_str(), response.size(), code_)) {
    LOG_ERR("SYNC", "POST /device/code failed (ok=%d status=%d)", ok, status);
    fail(FailureReason::REQUEST_CODE);
    return;
  }

  poller_.start(millis(), code_.interval, code_.expiresIn);
  enterQrScreen();
}

void SyncPairingActivity::enterQrScreen() {
  screen_ = Screen::QR_CODE;
  lastShownMinutesRemaining_ = -1;
  requestUpdate();
#ifdef CP_TEST_CONSOLE
  // Cheapest way for the host-side test harness to read the user code back
  // without OCR-ing a screenshot -- see scripts/device_tests/test_pairing.py.
  // Printed once, here, rather than from render() (which may repaint again
  // later purely for the countdown) so the harness sees exactly one line per
  // code issued.
  logSerial.printf("[TEST] {\"userCode\":\"%s\",\"expiresIn\":%u,\"interval\":%u}\n", code_.userCode,
                   (unsigned)code_.expiresIn, (unsigned)code_.interval);
#endif
}

void SyncPairingActivity::pollNow() {
  JsonDocument doc;
  doc["deviceCode"] = code_.deviceCode;
  std::string body;
  serializeJson(doc, body);

  const std::string url = SYNC_STORE.getBaseUrl() + "/device/token";
  std::string response;
  int status = -1;
  didNetworkWork_ = true;
  const bool ok = HttpDownloader::postJson(url, body, response, &status);
  LOG_DBG("SYNC", "Poll /device/token: ok=%d status=%d (heap: %u)", ok, status, (unsigned)ESP.getFreeHeap());

  if (ok && status == 200) {
    DeviceTokenResponse token;
    if (parseDeviceTokenSuccess(response.c_str(), response.size(), token)) {
      onPaired(token);
    } else {
      fail(FailureReason::POLL_ERROR);
    }
    return;
  }

  if (ok && status == 400) {
    const DeviceTokenPollError error = parseDeviceTokenPollError(response.c_str(), response.size());
    DevicePairingPollOutcome outcome;
    switch (error) {
      case DeviceTokenPollError::SLOW_DOWN:
        outcome = DevicePairingPollOutcome::SLOW_DOWN;
        break;
      case DeviceTokenPollError::ACCESS_DENIED:
        outcome = DevicePairingPollOutcome::ACCESS_DENIED;
        break;
      case DeviceTokenPollError::EXPIRED_TOKEN:
        outcome = DevicePairingPollOutcome::EXPIRED_TOKEN;
        break;
      case DeviceTokenPollError::AUTHORIZATION_PENDING:
      case DeviceTokenPollError::NONE:
      default:
        outcome = DevicePairingPollOutcome::AUTHORIZATION_PENDING;
        break;
    }
    poller_.onPollResult(millis(), outcome);
  } else {
    // Transport/server hiccup mid-poll: treat as a retry, not a hard
    // failure -- a single flaky request over a five-minute window should
    // not force the user to rescan a QR code. If connectivity never
    // recovers, the poller's own local expiry still ends the flow.
    poller_.onPollResult(millis(), DevicePairingPollOutcome::TRANSPORT_ERROR);
  }

  if (poller_.state() == DevicePairingPollState::DENIED) {
    fail(FailureReason::DENIED);
  } else if (poller_.state() == DevicePairingPollState::EXPIRED) {
    fail(FailureReason::EXPIRED);
  }
  // Otherwise still WAITING: no screen change, so no repaint (see the
  // "do not repaint on every poll" brief note).
}

void SyncPairingActivity::onPaired(const DeviceTokenResponse& token) {
  SYNC_STORE.setPairing(token.accessToken, token.deviceId, token.deviceName, token.accountEmail);
  accountEmail_ = token.accountEmail;
  pairedDeviceName_ = token.deviceName;
  screen_ = Screen::SUCCESS;
  requestUpdate();
}

void SyncPairingActivity::fail(FailureReason reason) {
  failureReason_ = reason;
  screen_ = Screen::FAILURE;
  requestUpdate();
}

void SyncPairingActivity::retry() {
  // Regenerates a fresh code (covers "expired_token: offer to generate a
  // new code") and re-checks Wi-Fi in case it dropped while waiting.
  screen_ = Screen::CONNECTING_WIFI;
  startWifi();
}

void SyncPairingActivity::updateCountdownIfChanged(const uint32_t nowMs) {
  const uint32_t secondsLeft = poller_.secondsRemaining(nowMs);
  // Round up so "59 seconds left" reads as "1 min", not "0 min", and the
  // display never claims more time than the server actually granted.
  const int32_t minutesLeft = static_cast<int32_t>((secondsLeft + 59) / 60);
  if (minutesLeft != lastShownMinutesRemaining_) {
    lastShownMinutesRemaining_ = minutesLeft;
    requestUpdate();
  }
}

void SyncPairingActivity::loop() {
  if (screen_ == Screen::QR_CODE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      ActivityResult cancelled;
      cancelled.isCancelled = true;
      setResult(std::move(cancelled));
      finish();
      return;
    }
    const uint32_t now = millis();
    updateCountdownIfChanged(now);
    if (poller_.dueForPoll(now)) {
      pollNow();
      return;
    }
    if (poller_.state() == DevicePairingPollState::EXPIRED) {
      fail(FailureReason::EXPIRED);
    }
    return;
  }

  if (screen_ == Screen::SUCCESS) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
    return;
  }

  if (screen_ == Screen::FAILURE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      ActivityResult cancelled;
      cancelled.isCancelled = true;
      setResult(std::move(cancelled));
      finish();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      retry();
    }
    return;
  }
}

void SyncPairingActivity::render(RenderLock&&) {
  // CONNECTING_WIFI is the WifiSelectionActivity sub-activity's screen; we
  // never reach here for it (onEnter() starts it before this activity's own
  // requestUpdate() ever fires), but skip defensively like WifiSelectionActivity
  // does for its own sub-activity transitions.
  if (screen_ == Screen::CONNECTING_WIFI) return;

  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_ACCOUNT_SYNC));

  const auto height = renderer.getLineHeight(UI_10_FONT_ID);

  if (screen_ == Screen::REQUESTING_CODE) {
    const auto top = (pageHeight - height) / 2;
    UITheme::drawCenteredText(renderer, Rect{0, 0, pageWidth, pageHeight}, UI_10_FONT_ID, top, tr(STR_REQUESTING_CODE));
  } else if (screen_ == Screen::QR_CODE) {
    const int startY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    // Reserve room below the QR for the instruction line, the user code, and
    // the countdown -- three UI_10 lines plus spacing.
    const int textBlockHeight = height * 3 + metrics.verticalSpacing * 3;
    const int qrSide = pageWidth - 40;
    const int qrHeight = pageHeight - metrics.topPadding - metrics.headerHeight - metrics.verticalSpacing * 2 -
                        textBlockHeight - 20;
    const Rect qrBounds(20, startY, qrSide, qrHeight);
    QrUtils::drawQrCode(renderer, qrBounds, code_.verificationUriComplete);

    int textY = startY + qrHeight + metrics.verticalSpacing;
    UITheme::drawCenteredText(renderer, Rect{0, 0, pageWidth, pageHeight}, UI_10_FONT_ID, textY, tr(STR_SCAN_TO_PAIR));
    textY += height + metrics.verticalSpacing;
    std::string codeLine = std::string(tr(STR_OR_ENTER_CODE)) + " " + code_.userCode;
    UITheme::drawCenteredText(renderer, Rect{0, 0, pageWidth, pageHeight}, UI_10_FONT_ID, textY, codeLine.c_str(), true,
                              EpdFontFamily::BOLD);
    textY += height + metrics.verticalSpacing;
    char countdown[64];
    if (lastShownMinutesRemaining_ <= 0) {
      snprintf(countdown, sizeof(countdown), "%s", tr(STR_EXPIRES_SOON));
    } else {
      snprintf(countdown, sizeof(countdown), tr(STR_EXPIRES_IN_MINUTES), lastShownMinutesRemaining_);
    }
    UITheme::drawCenteredText(renderer, Rect{0, 0, pageWidth, pageHeight}, UI_10_FONT_ID, textY, countdown);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (screen_ == Screen::SUCCESS) {
    const auto top = (pageHeight - height * 3) / 2;
    UITheme::drawCenteredText(renderer, Rect{0, 0, pageWidth, pageHeight}, UI_12_FONT_ID, top, tr(STR_DEVICE_PAIRED),
                              true, EpdFontFamily::BOLD);
    char signedInAs[192];
    snprintf(signedInAs, sizeof(signedInAs), tr(STR_SIGNED_IN_AS), accountEmail_.c_str());
    UITheme::drawCenteredText(renderer, Rect{0, 0, pageWidth, pageHeight}, UI_10_FONT_ID, top + height + 10,
                              signedInAs);
    if (!pairedDeviceName_.empty()) {
      UITheme::drawCenteredText(renderer, Rect{0, 0, pageWidth, pageHeight}, UI_10_FONT_ID, top + (height + 10) * 2,
                                pairedDeviceName_.c_str());
    }
    const auto labels = mappedInput.mapLabels("", tr(STR_DONE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (screen_ == Screen::FAILURE) {
    const auto top = (pageHeight - height * 2) / 2;
    UITheme::drawCenteredText(renderer, Rect{0, 0, pageWidth, pageHeight}, UI_12_FONT_ID, top - 20,
                              tr(STR_PAIRING_FAILED_TITLE), true, EpdFontFamily::BOLD);
    std::string message = std::string(failureMessage(failureReason_)) + " (" + failureShortCode(failureReason_) + ")";
    UITheme::drawCenteredWrappedText(renderer, Rect{20, top + 10, pageWidth - 40, height * 3}, UI_10_FONT_ID,
                                     message.c_str(), 3);
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
