#include "RemoteProgressCheck.h"

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cassert>

#include "KOReaderCredentialStore.h"
#include "SleepProgressSync.h"
#include "SleepWifiBackoffPolicy.h"
#include "SyncCredentialStore.h"
#include "SyncTriggerPolicy.h"
#include "activities/RenderLock.h"

namespace remote_progress {

namespace {

// Same 12288 DownloadQueue uses, and for the same measured reason: a wolfSSL
// handshake plus the HTTP layer above it overflows the 4096 AGENTS.md's table
// suggests for network work, and crashes the device at the first byte.
constexpr uint32_t WORKER_STACK_BYTES = 12288;

class Checker {
 public:
  Checker() : mutex_(xSemaphoreCreateMutex()) { assert(mutex_ != nullptr); }

  void start(const std::string& documentHash) {
    if (documentHash.empty()) return;
    if (!SYNC_STORE.isPaired() || !KOREADER_STORE.hasEffectiveCredentials()) return;

    Lock lock(mutex_);
    if (taskHandle_ != nullptr) return;  // one at a time

    requestedHash_ = documentHash;
    ready_ = false;
    haveProgress_ = false;
    aborted_ = false;
    const BaseType_t created =
        xTaskCreate(&Checker::taskTrampoline, "RemoteProgress", WORKER_STACK_BYTES, this, 1, &taskHandle_);
    if (created != pdPASS) {
      LOG_ERR("RPC", "Failed to create worker task");
      taskHandle_ = nullptr;
    }
  }

  bool consume(const std::string& documentHash, KOReaderProgress& outProgress, bool& outHaveProgress) {
    Lock lock(mutex_);
    if (!ready_ || requestedHash_ != documentHash) return false;
    ready_ = false;
    outHaveProgress = haveProgress_;
    if (haveProgress_) outProgress = progress_;
    return true;
  }

  void discard() {
    Lock lock(mutex_);
    // Not a stop-and-join: waiting for the worker here would block the loop
    // task -- and this is called from ~EpubReaderActivity, which power-off
    // runs on the way into sleep. The worker reads this flag at every stage
    // boundary and bails silently, so the residual window is one stage: a
    // TLS handshake or HTTP request already in flight still has to finish or
    // hit its own AUTO_SYNC_TIMEOUT_MS deadline. What the flag does buy is
    // that the worker will not *start* a Wi-Fi search or a request after
    // this, so it cannot fight the before-sleep push for the radio for more
    // than that one already-bounded stage. On abort the worker also leaves
    // the radio alone rather than tearing it down, because whoever is
    // shutting this check down is very likely the one that wants it up.
    aborted_ = true;
    ready_ = false;
    haveProgress_ = false;
    requestedHash_.clear();
  }

 private:
  struct Lock {
    SemaphoreHandle_t handle;
    explicit Lock(SemaphoreHandle_t h) : handle(h) { xSemaphoreTake(handle, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(handle); }
  };

  static void taskTrampoline(void* param) { static_cast<Checker*>(param)->run(); }

  bool aborted() {
    Lock lock(mutex_);
    return aborted_;
  }

  // Publishes nothing and releases the task slot. Used by every bail-out
  // path, so a discarded check can never leave start() refusing the next
  // book's fetch forever.
  void finishSilently() {
    {
      Lock lock(mutex_);
      taskHandle_ = nullptr;
    }
    logStackHighWater();
    vTaskDelete(nullptr);
  }

  static void logStackHighWater() {
    // uxTaskGetStackHighWaterMark returns words, not bytes -- same conversion
    // DownloadQueue.cpp does for the same diagnostic, so the two numbers can
    // be compared against the same 12288-byte stack.
    LOG_DBG("RPC", "Worker stack high water: %u bytes",
            (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
  }

  // Tears the radio back down when this worker is the one that raised it.
  // connectToSavedWifi() explicitly does not do this -- it was written for
  // enterDeepSleep(), which tears WiFi down unconditionally a few lines
  // later. A reader stays alive for hours instead, and
  // HalPowerManager::setPowerSaving() refuses to drop the C3 to
  // LOW_POWER_FREQ while WiFi.getMode() != WIFI_MODE_NULL, so leaving the
  // stack up would cost the rest of the session's idle power, not just the
  // fetch. Mirrors enterDeepSleep()'s own teardown. Runs even when the
  // search failed: connectToSavedWifi() sets WIFI_STA before it starts
  // looking, so a failed attempt leaves the mode set too.
  static void tearDownWifi() {
    if (WiFi.getMode() == WIFI_MODE_NULL) return;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    LOG_DBG("RPC", "Radio released after remote progress check");
  }

  void run() {
    std::string hash;
    {
      Lock lock(mutex_);
      hash = requestedHash_;
    }

    if (aborted()) return finishSilently();

    // Checked before the radio goes up, not just inside the client: a
    // handshake that is going to be refused for memory should not cost the
    // battery a Wi-Fi association first.
    if (ESP.getFreeHeap() < MIN_FREE_FOR_TLS || ESP.getMaxAllocHeap() < MIN_BLOCK_FOR_TLS) {
      LOG_DBG("RPC", "Skipping remote progress check: heap too low (%u free, %u max alloc)",
              (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
      return finishSilently();
    }

    if (aborted()) return finishSilently();

    // Only an attempt this worker actually makes touches the shared back-off
    // counter, same rule HomeActivity's bring-up follows: WiFi that is
    // already up belongs to somebody else, and connectToSavedWifi() no-ops
    // on it.
    const bool broughtWifiUp = WiFi.status() != WL_CONNECTED;
    sleep_wifi_backoff::State backoffState;
    if (broughtWifiUp) {
      backoffState = loadBackoffState();
      if (!sleep_wifi_backoff::shouldAttempt(backoffState)) {
        // The device wakes straight into the reader, so without this gate a
        // reader carried out of range pays a full 2.5s Wi-Fi search on every
        // single book open, forever -- there is no clock to expire it. See
        // lib/SyncManifest/SleepWifiBackoffPolicy.h.
        LOG_DBG("RPC", "Skipping remote progress check: backed off (%u skip(s) left after %u failure(s))",
                backoffState.skipsRemaining, backoffState.consecutiveFailures);
        saveBackoffState(sleep_wifi_backoff::afterSkippedAttempt(backoffState));
        return finishSilently();
      }
    }

    bool cancelled = false;
    // callerHoldsRenderLock=false: this runs on its own task, holding no
    // render lock. The SD read for Wi-Fi credentials inside takes its own.
    // pollPowerButton=false: gpio.update() belongs to the loop task, and
    // calling it from here would recompute the button edge words underneath
    // the reader and swallow a page turn -- see connectToSavedWifi()'s
    // header comment. This worker needs no escape hatch: it is bounded at
    // WIFI_CONNECT_TIMEOUT_MS and silent either way.
    const bool wifiConnected =
        sleep_progress_sync::connectToSavedWifi(cancelled, /*callerHoldsRenderLock=*/false, /*pollPowerButton=*/false);

    if (!wifiConnected) {
      LOG_DBG("RPC", "No Wi-Fi for remote progress check; staying silent");
      // Nothing at all once aborted: the radio belongs to whoever asked for
      // the abort, and the back-off write would put an SD write in front of
      // a power-off that is already under way.
      if (broughtWifiUp && !aborted()) {
        tearDownWifi();
        saveBackoffState(sleep_wifi_backoff::afterAttempt(backoffState, /*reached=*/false));
      }
      return finishSilently();
    }

    if (aborted()) return finishSilently();

    KOReaderProgress fetched;
    bool have = false;
    const auto result = KOReaderSyncClient::getProgress(hash, fetched, sync_trigger::AUTO_SYNC_TIMEOUT_MS);
    if (result == KOReaderSyncClient::OK) {
      have = true;
      LOG_DBG("RPC", "Remote progress: %.4f from '%s'", fetched.percentage, fetched.deviceId.c_str());
    } else {
      LOG_DBG("RPC", "Remote progress check: %s", KOReaderSyncClient::errorString(result));
    }

    if (broughtWifiUp && !aborted()) {
      tearDownWifi();
      // NETWORK_ERROR alone means the access point associated but nothing
      // behind it answered; any other error still proves a real response came
      // back. Same reading SleepProgressSync applies to its own upload.
      saveBackoffState(sleep_wifi_backoff::afterAttempt(
          backoffState, sleep_wifi_backoff::reachedNetwork(true, result == KOReaderSyncClient::NETWORK_ERROR)));
    }

    {
      Lock lock(mutex_);
      taskHandle_ = nullptr;
      // The book closed while this was in flight: publishing now would let the
      // next book inherit this answer if its hash happened to match.
      if (!aborted_) {
        progress_ = std::move(fetched);
        haveProgress_ = have;
        ready_ = true;
      }
    }
    logStackHighWater();
    vTaskDelete(nullptr);
  }

  // The back-off counter lives in CrossPointState on SD. Reading it is free,
  // but saving it can write the file, and SD shares the display's SPI bus --
  // hence the RenderLock, the same lock connectToSavedWifi() takes around its
  // own credential read on this task.
  static sleep_wifi_backoff::State loadBackoffState() { return sleep_progress_sync::loadWifiBackoffState(); }

  static void saveBackoffState(const sleep_wifi_backoff::State& state) {
    RenderLock lock;
    sleep_progress_sync::saveWifiBackoffState(state);
  }

  // Mirrors KOReaderSyncClient.cpp's own admission bar; duplicated rather
  // than exported because the client's copy is the one that actually
  // enforces it and must stay authoritative there.
  static constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
  static constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;

  SemaphoreHandle_t mutex_;
  TaskHandle_t taskHandle_ = nullptr;
  std::string requestedHash_;
  KOReaderProgress progress_;
  bool haveProgress_ = false;
  bool ready_ = false;
  // Set by discard() when the reader closes; read by the worker at every
  // stage boundary. Guarded by mutex_ like every other field here.
  bool aborted_ = false;
};

Checker& checker() {
  static Checker instance;
  return instance;
}

}  // namespace

void start(const std::string& documentHash) { checker().start(documentHash); }

bool consume(const std::string& documentHash, KOReaderProgress& outProgress, bool& outHaveProgress) {
  return checker().consume(documentHash, outProgress, outHaveProgress);
}

void discard() { checker().discard(); }

}  // namespace remote_progress
