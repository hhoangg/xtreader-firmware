#include "SleepProgressSync.h"

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>

#include <string>

#include "WifiCredentialStore.h"
#include "activities/Activity.h"  // completes Activity before ActivityManager.h's inline ctor needs unique_ptr<Activity>
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "util/TaskWatchdog.h"

namespace sleep_progress_sync {

namespace {

// Per-network slice of WIFI_CONNECT_TIMEOUT_MS, so one dead saved network
// cannot alone burn the whole budget and leave no time to try a second one.
constexpr unsigned long PER_NETWORK_TIMEOUT_MS = 4000;

// Mirrors main.cpp's CP_TEST_CONSOLE-only testConsoleConnectWifi() (same
// last-connected-SSID-first order, same per-network polling loop), but is
// compiled into every build -- this is the one enterDeepSleep() actually
// calls in production, not a test-console diagnostic.
bool tryCredential(const std::string& ssid, const std::string& password, const unsigned long overallDeadlineMs) {
  WiFi.disconnect();
  delay(50);
  if (!password.empty()) {
    WiFi.begin(ssid.c_str(), password.c_str());
  } else {
    WiFi.begin(ssid.c_str());
  }

  const unsigned long perNetworkDeadline = millis() + PER_NETWORK_TIMEOUT_MS;
  while (static_cast<long>(millis() - perNetworkDeadline) < 0 &&
        static_cast<long>(millis() - overallDeadlineMs) < 0) {
    resetTaskWatchdogIfSubscribed();
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) return true;
    if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) break;
    delay(50);
  }
  return false;
}

bool connectToSavedWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  {
    // SD card access shares SPI with the display; matches
    // WifiSelectionActivity::onEnter()'s use of the same lock.
    RenderLock lock;
    WIFI_STORE.loadFromFile();
  }

  const size_t savedCount = WIFI_STORE.getCredentialCount();
  if (savedCount == 0) {
    LOG_DBG("SLPSYNC", "No saved WiFi credentials, skipping before-sleep sync");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);       // credentials are managed by WifiCredentialStore, not SDK NVS
  WiFi.disconnect(true, true);  // abort any in-progress SDK auto-connect
  delay(100);

  const unsigned long overallDeadline = millis() + WIFI_CONNECT_TIMEOUT_MS;

  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  bool triedLast = false;
  if (!lastSsid.empty()) {
    const auto cred = WIFI_STORE.findCredential(lastSsid);
    if (cred) {
      triedLast = true;
      if (tryCredential(cred->ssid, cred->password, overallDeadline)) return true;
    }
  }

  for (size_t i = 0; i < savedCount && static_cast<long>(millis() - overallDeadline) < 0; i++) {
    const auto cred = WIFI_STORE.getCredentialAt(i);
    if (!cred || (triedLast && cred->ssid == lastSsid)) continue;
    if (tryCredential(cred->ssid, cred->password, overallDeadline)) return true;
  }

  return false;
}

#ifdef CP_TEST_CONSOLE
void logHeapJson(const char* when) {
  logSerial.printf("[TEST] {\"stage\":\"sleep_sync_heap\",\"when\":\"%s\",\"freeHeap\":%u,\"maxAllocHeap\":%u}\n",
                    when, static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
}
#endif

}  // namespace

bool trySyncBeforeSleep() {
  if (!activityManager.readerHasUnsyncedProgress()) return false;

#ifdef CP_TEST_CONSOLE
  logHeapJson("start");
#endif

  if (!connectToSavedWifi()) {
    LOG_DBG("SLPSYNC", "No WiFi for before-sleep sync; progress goes up another time");
#ifdef CP_TEST_CONSOLE
    logHeapJson("no_wifi");
#endif
    return false;
  }

  const bool sent = activityManager.syncReaderProgressForSleep();
  LOG_DBG("SLPSYNC", "Before-sleep sync %s", sent ? "sent" : "failed");

#ifdef CP_TEST_CONSOLE
  logHeapJson("done");
#endif

  return sent;
}

}  // namespace sleep_progress_sync
