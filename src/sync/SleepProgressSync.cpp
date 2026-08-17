#include "SleepProgressSync.h"

#include <Arduino.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <WiFi.h>

#include <string>

#include "CrossPointState.h"
#include "KOReaderSyncClient.h"
#include "SleepWifiBackoffPolicy.h"
#include "SyncTriggerPolicy.h"
#include "Telemetry.h"
#include "WifiCredentialStore.h"
#include "activities/RenderLock.h"
#include "util/TaskWatchdog.h"

namespace sleep_progress_sync {

namespace {

// Per-network slice of WIFI_CONNECT_TIMEOUT_MS, so one dead saved network
// cannot alone burn the whole budget and leave no time to try a second one.
constexpr unsigned long PER_NETWORK_TIMEOUT_MS = 1500;

// "Let the power button win" (task brief): a fresh press during the Wi-Fi
// search is the owner's escape hatch if this ever takes longer than they are
// willing to wait, regardless of budget/back-off tuning. gpio.update() is
// safe to call repeatedly here -- this runs synchronously on the loop task
// (enterDeepSleep() -> here), the same task that would otherwise be driving
// MappedInputManager::update() -> gpio.update() every frame; nothing else
// polls it concurrently while this loop blocks. wasPressed() is an edge, so
// a button still held from the original power-off gesture does not
// re-trigger this -- it takes an actual release-then-press.
bool powerButtonPressedAgain() {
  gpio.update();
  return gpio.wasPressed(HalGPIO::BTN_POWER);
}

// Mirrors main.cpp's CP_TEST_CONSOLE-only testConsoleConnectWifi() (same
// last-connected-SSID-first order, same per-network polling loop), but is
// compiled into every build -- this is the one enterDeepSleep() actually
// calls in production, not a test-console diagnostic.
bool tryCredential(const std::string& ssid, const std::string& password, const unsigned long overallDeadlineMs,
                   bool& cancelled) {
  WiFi.disconnect();
  delay(50);
  if (!password.empty()) {
    WiFi.begin(ssid.c_str(), password.c_str());
  } else {
    WiFi.begin(ssid.c_str());
  }

  const unsigned long perNetworkDeadline = millis() + PER_NETWORK_TIMEOUT_MS;
  while (static_cast<long>(millis() - perNetworkDeadline) < 0 && static_cast<long>(millis() - overallDeadlineMs) < 0) {
    resetTaskWatchdogIfSubscribed();
    if (powerButtonPressedAgain()) {
      cancelled = true;
      return false;
    }
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) return true;
    if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) break;
    delay(50);
  }
  return false;
}

#ifdef CP_TEST_CONSOLE
// Set by CMD:SLEEPSYNCBENCH before calling trySyncBeforeSleep(), so
// connectToSavedWifi() below substitutes one deliberately nonexistent
// credential for WifiCredentialStore's real saved list. Lets the exact
// production path (including the back-off state machine) be timed on
// hardware without the owner needing to disable their real router. Cleared
// again immediately after. See main.cpp's testConsoleSleepSyncBench().
bool benchForceBogusNetwork = false;
constexpr char BENCH_BOGUS_SSID[] = "CP-BENCH-UNREACHABLE-NETWORK";
#endif

#ifdef CP_TEST_CONSOLE
void logHeapJson(const char* when) {
  logSerial.printf("[TEST] {\"stage\":\"sleep_sync_heap\",\"when\":\"%s\",\"freeHeap\":%u,\"maxAllocHeap\":%u}\n", when,
                   static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

void logStageJson(const char* stage, unsigned long elapsedMs) {
  logSerial.printf("[TEST] {\"stage\":\"%s\",\"elapsedMs\":%lu}\n", stage, elapsedMs);
}
#endif

}  // namespace

bool connectToSavedWifi(bool& cancelled, bool callerHoldsRenderLock) {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);       // credentials are managed by WifiCredentialStore, not SDK NVS
  WiFi.disconnect(true, true);  // abort any in-progress SDK auto-connect
  delay(100);

  const unsigned long overallDeadline = millis() + WIFI_CONNECT_TIMEOUT_MS;

#ifdef CP_TEST_CONSOLE
  if (benchForceBogusNetwork) {
    return tryCredential(BENCH_BOGUS_SSID, "", overallDeadline, cancelled);
  }
#endif

  // SD card access shares SPI with the display; matches
  // WifiSelectionActivity::onEnter()'s use of the same lock. Skipped when
  // the caller already holds it -- see this function's header comment for
  // why re-taking it here would deadlock that caller's task against itself.
  if (callerHoldsRenderLock) {
    WIFI_STORE.loadFromFile();
  } else {
    RenderLock lock;
    WIFI_STORE.loadFromFile();
  }

  const size_t savedCount = WIFI_STORE.getCredentialCount();
  if (savedCount == 0) {
    LOG_DBG("SLPSYNC", "No saved WiFi credentials, skipping WiFi bring-up");
    return false;
  }

  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  bool triedLast = false;
  if (!lastSsid.empty()) {
    const auto cred = WIFI_STORE.findCredential(lastSsid);
    if (cred) {
      triedLast = true;
      if (tryCredential(cred->ssid, cred->password, overallDeadline, cancelled)) return true;
      if (cancelled) return false;
    }
  }

  for (size_t i = 0; i < savedCount && static_cast<long>(millis() - overallDeadline) < 0; i++) {
    const auto cred = WIFI_STORE.getCredentialAt(i);
    if (!cred || (triedLast && cred->ssid == lastSsid)) continue;
    if (tryCredential(cred->ssid, cred->password, overallDeadline, cancelled)) return true;
    if (cancelled) return false;
  }

  return false;
}

sleep_wifi_backoff::State loadWifiBackoffState() {
  return {APP_STATE.sleepWifiConsecutiveFailures, APP_STATE.sleepWifiSkipsRemaining};
}

// No-op (no SD write) when the state did not actually change, e.g. an
// already-clean device that skipped straight past the backoff check.
void saveWifiBackoffState(const sleep_wifi_backoff::State& state) {
  if (APP_STATE.sleepWifiConsecutiveFailures == state.consecutiveFailures &&
      APP_STATE.sleepWifiSkipsRemaining == state.skipsRemaining) {
    return;
  }
  APP_STATE.sleepWifiConsecutiveFailures = state.consecutiveFailures;
  APP_STATE.sleepWifiSkipsRemaining = state.skipsRemaining;
  APP_STATE.saveToFile();
}

bool trySyncBeforeSleep(const KOReaderProgress& progress) {
#ifdef CP_TEST_CONSOLE
  logHeapJson("start");
  const unsigned long overallStart = millis();
#endif

  const sleep_wifi_backoff::State backoffState = loadWifiBackoffState();
  if (!sleep_wifi_backoff::shouldAttempt(backoffState)) {
    LOG_DBG("SLPSYNC",
            "Skipping before-sleep Wi-Fi attempt: backed off (%u skip(s) left after %u consecutive failure(s))",
            backoffState.skipsRemaining, backoffState.consecutiveFailures);
    saveWifiBackoffState(sleep_wifi_backoff::afterSkippedAttempt(backoffState));
#ifdef CP_TEST_CONSOLE
    logStageJson("sleep_sync_backed_off", millis() - overallStart);
#endif
    return false;
  }

#ifdef CP_TEST_CONSOLE
  const unsigned long wifiStart = millis();
#endif
  bool cancelled = false;
  // Runs on the main/loop task (enterDeepSleep() -> here), never the render
  // task -- see connectToSavedWifi()'s header comment for why this matters.
  const bool wifiConnected = connectToSavedWifi(cancelled, /*callerHoldsRenderLock=*/false);
#ifdef CP_TEST_CONSOLE
  logStageJson("sleep_sync_wifi", millis() - wifiStart);
#endif

  if (cancelled) {
    // Power button wins: not evidence of "no Wi-Fi here", just an owner who
    // wants the device off now -- leave the back-off state untouched.
    LOG_DBG("SLPSYNC", "Before-sleep Wi-Fi attempt cancelled by power button");
    return false;
  }

  if (!wifiConnected) {
    LOG_DBG("SLPSYNC", "No WiFi for before-sleep sync; progress goes up another time");
    saveWifiBackoffState(sleep_wifi_backoff::afterAttempt(backoffState, false));
#ifdef CP_TEST_CONSOLE
    logHeapJson("no_wifi");
#endif
    return false;
  }

  if (powerButtonPressedAgain()) {
    // Same "power button wins" reasoning as the cancellation branch above:
    // not evidence of a reachability problem, just an owner who wants the
    // device off now -- leave the back-off state untouched (it is saved
    // below, only once the upload actually ran).
    LOG_DBG("SLPSYNC", "Before-sleep sync cancelled by power button after Wi-Fi connected; skipping upload");
    return false;
  }

#ifdef CP_TEST_CONSOLE
  const unsigned long uploadStart = millis();
#endif
  // AUTO_SYNC_TIMEOUT_MS, not KOReaderSyncClient's own explicit-action
  // default: nobody is watching this attempt, so a captive portal or
  // black-holed server (Wi-Fi associates, then the request itself never
  // answers -- see this task's report) must not add its own multi-second
  // stall on top of the Wi-Fi budget already spent above.
  const auto result = KOReaderSyncClient::updateProgress(progress, sync_trigger::AUTO_SYNC_TIMEOUT_MS);
  const bool sent = (result == KOReaderSyncClient::OK);
  if (!sent) {
    LOG_ERR("KOSync", "Sleep sync failed: %s", KOReaderSyncClient::errorString(result));
  }
#ifdef CP_TEST_CONSOLE
  logStageJson("sleep_sync_upload", millis() - uploadStart);
#endif
  LOG_DBG("SLPSYNC", "Before-sleep sync %s", sent ? "sent" : "failed");

  // NETWORK_ERROR specifically -- not just "sent != OK" -- is the signal
  // that Wi-Fi associated but nothing behind it actually answered (a
  // captive portal or black-holed server). Any other outcome (AUTH_FAILED,
  // SERVER_ERROR, ...) still proves a real HTTP response came back, i.e.
  // the network itself was fine, so it must not earn a skip -- see
  // SleepWifiBackoffPolicy.h's reachedNetwork().
  const bool reached = sleep_wifi_backoff::reachedNetwork(wifiConnected, result == KOReaderSyncClient::NETWORK_ERROR);
  saveWifiBackoffState(sleep_wifi_backoff::afterAttempt(backoffState, reached));

  if (!reached) {
    // The heartbeat below would hit the same unreachable network the upload
    // above just did -- skip it rather than paying its own deadline for a
    // result already known. Best-effort like the upload itself, so this is
    // never surfaced beyond a log line.
    LOG_DBG("SLPSYNC", "Skipping heartbeat piggyback: before-sleep sync could not reach the network");
#ifdef CP_TEST_CONSOLE
    logStageJson("sleep_sync_heartbeat_skipped", 0);
    logHeapJson("done");
    logStageJson("sleep_sync_total", millis() - overallStart);
#endif
    return sent;
  }

  // WiFi is already up here for the progress upload above -- the other of
  // the two moments (task brief) a heartbeat can ride along without paying
  // its own WiFi cost. Battery is read now, at power-off, rather than at the
  // next wake: this is the last data point the owner's dashboard gets before
  // the device may sit idle for days, so it should reflect the state the
  // device is actually going dark in. Best-effort like the progress upload
  // itself: a failed heartbeat must not affect this sleep, only be logged.
  // Automatic bound, same reasoning as the upload above.
  telemetry::HeartbeatInfo heartbeatInfo = telemetry::currentDeviceHeartbeatInfo();
  heartbeatInfo.lastSyncStatus = sent ? "ok" : "failed";
  const telemetry::TelemetryResult heartbeatResult =
      telemetry::sendHeartbeat(heartbeatInfo, sync_trigger::AUTO_SYNC_TIMEOUT_MS);
  if (!heartbeatResult.ok) {
    LOG_DBG("SLPSYNC", "Heartbeat piggybacked on before-sleep sync failed (error=%s status=%d) -- diagnostics only",
            heartbeatResult.error.c_str(), heartbeatResult.httpStatus);
  }

#ifdef CP_TEST_CONSOLE
  logHeapJson("done");
  logStageJson("sleep_sync_total", millis() - overallStart);
#endif

  return sent;
}

#ifdef CP_TEST_CONSOLE
bool benchTrySyncAgainstBogusNetwork(const KOReaderProgress& progress) {
  benchForceBogusNetwork = true;
  const bool sent = trySyncBeforeSleep(progress);
  benchForceBogusNetwork = false;
  return sent;
}

bool benchTrySyncAgainstBlackHole(const KOReaderProgress& progress) {
  // Real saved Wi-Fi (benchForceBogusNetwork stays false), so association
  // succeeds normally -- only the KOSync upload target is diverted, standing
  // in for a captive portal or a server that accepted the TCP connection
  // and never answered. See KOReaderSyncClient::setTestBlackHoleOverride().
  KOReaderSyncClient::setTestBlackHoleOverride(true);
  const bool sent = trySyncBeforeSleep(progress);
  KOReaderSyncClient::setTestBlackHoleOverride(false);
  return sent;
}
#endif

}  // namespace sleep_progress_sync
