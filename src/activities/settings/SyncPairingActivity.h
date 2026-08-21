#pragma once

#include <cstdint>
#include <string>

#include "DevicePairingPoller.h"
#include "DevicePairingProtocol.h"
#include "activities/Activity.h"

/**
 * OAuth 2.0 Device Authorization Grant (RFC 8628) pairing flow: brings up
 * Wi-Fi, requests a device/user code pair from crosspoint-sync, shows a QR
 * code (+ the user code as text, a fallback for a dim screen) while polling
 * for approval, and stores the resulting bearer token in SyncCredentialStore
 * (NVS) on success.
 *
 * See crosspoint-sync docs/API.md's "Pairing" section for the wire protocol
 * this drives; DevicePairingProtocol/DevicePairingPoller (lib/DevicePairing)
 * hold the host-testable parsing and poll-timing logic this activity is a
 * thin, hardware-driving shell around.
 */
class SyncPairingActivity final : public Activity {
 public:
  explicit SyncPairingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum class Screen : uint8_t {
    CONNECTING_WIFI,  // WifiSelectionActivity sub-activity owns the screen
    REQUESTING_CODE,  // brief beat between Wi-Fi coming up and the QR appearing
    QR_CODE,          // QR + userCode + countdown; polling happens silently here
    SUCCESS,
    FAILURE,
  };

  // Short codes shown on the Failure screen next to a plain sentence -- see
  // the brief: the person holding this device cannot debug it, so the
  // message must tell them to ask whoever set it up, plus a code that
  // person can act on. Numbered rather than named after the cause so the
  // displayed string stays stable if the wording is retranslated.
  enum class FailureReason : uint8_t {
    WIFI,          // P-01: could not bring up Wi-Fi
    REQUEST_CODE,  // P-02: POST /device/code failed (network or server)
    DENIED,        // P-03: access_denied
    EXPIRED,       // P-04: expired_token, or the local expiresIn timer ran out
    POLL_ERROR,    // P-05: POST /device/token failed for some other reason
  };

  void startWifi();
  void onWifiResult(bool connected);
  void requestCode();
  void enterQrScreen();
  void pollNow();
  void onPaired(const DeviceTokenResponse& token);
  void fail(FailureReason reason);
  void retry();
  void updateCountdownIfChanged(uint32_t nowMs);

  static const char* failureShortCode(FailureReason reason);
  static const char* failureMessage(FailureReason reason);

  Screen screen_ = Screen::CONNECTING_WIFI;
  DevicePairingPoller poller_;
  DeviceCodeResponse code_;
  FailureReason failureReason_ = FailureReason::WIFI;
  std::string accountEmail_;
  std::string pairedDeviceName_;

  // Minutes-remaining last painted on the QR screen; a repaint is triggered
  // only when this changes, so the poll loop (every `interval` seconds)
  // doesn't flash the e-ink display on every tick -- see the "do not
  // repaint on every poll" note in the pairing brief.
  int32_t lastShownMinutesRemaining_ = -1;

  // Set once a network call has actually been attempted (POST /device/code
  // or a poll), so onExit() knows whether to disconnect + silentRestart()
  // to release TLS-fragmented heap.
  bool didNetworkWork_ = false;
};
