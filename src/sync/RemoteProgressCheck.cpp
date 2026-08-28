#include "RemoteProgressCheck.h"

#include <Arduino.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cassert>

#include "KOReaderCredentialStore.h"
#include "SleepProgressSync.h"
#include "SyncCredentialStore.h"
#include "SyncTriggerPolicy.h"

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

  void run() {
    std::string hash;
    {
      Lock lock(mutex_);
      hash = requestedHash_;
    }

    KOReaderProgress fetched;
    bool have = false;

    // Checked before the radio goes up, not just inside the client: a
    // handshake that is going to be refused for memory should not cost the
    // battery a Wi-Fi association first.
    if (ESP.getFreeHeap() < MIN_FREE_FOR_TLS || ESP.getMaxAllocHeap() < MIN_BLOCK_FOR_TLS) {
      LOG_DBG("RPC", "Skipping remote progress check: heap too low (%u free, %u max alloc)",
              (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    } else {
      bool cancelled = false;
      // false: this runs on its own task, holding no render lock. The SD read
      // for Wi-Fi credentials inside takes its own.
      if (!sleep_progress_sync::connectToSavedWifi(cancelled, /*callerHoldsRenderLock=*/false)) {
        LOG_DBG("RPC", "No Wi-Fi for remote progress check; staying silent");
      } else {
        const auto result = KOReaderSyncClient::getProgress(hash, fetched, sync_trigger::AUTO_SYNC_TIMEOUT_MS);
        if (result == KOReaderSyncClient::OK) {
          have = true;
          LOG_DBG("RPC", "Remote progress: %.4f from '%s'", fetched.percentage, fetched.deviceId.c_str());
        } else {
          LOG_DBG("RPC", "Remote progress check: %s", KOReaderSyncClient::errorString(result));
        }
      }
    }

    {
      Lock lock(mutex_);
      progress_ = std::move(fetched);
      haveProgress_ = have;
      ready_ = true;
      taskHandle_ = nullptr;
    }
    LOG_DBG("RPC", "Worker stack high water: %u", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    vTaskDelete(nullptr);
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
