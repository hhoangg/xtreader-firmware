#include "DownloadQueue.h"

#include <Arduino.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cassert>

#include "BookDownloader.h"
#include "SyncCredentialStore.h"
#include "SyncManifest.h"

namespace download_queue {

namespace {

// A wolfSSL handshake plus the HTTP and SD layers above it overflows the 4096
// bytes CLAUDE.md's table suggests for network work: on hardware that crashed
// the device outright, at the first byte of the first download. The manifest
// sync path survives on the same libraries only because it runs on the render
// task, whose stack is far larger. Measured high-water is logged at the end of
// every item so this stays honest rather than becoming another guess.
constexpr uint32_t WORKER_STACK_BYTES = 12288;
// How long the worker sleeps between checks while paused (queue non-empty
// but the safety check says "not now") or once the queue is empty and it is
// about to exit -- short enough that a resumed sync starts promptly, long
// enough not to spin.
constexpr TickType_t WORKER_IDLE_DELAY = pdMS_TO_TICKS(1000);

struct HeapPair {
  uint32_t freeHeap;
  uint32_t maxAllocHeap;
};

HeapPair sampleHeap() {
  return HeapPair{static_cast<uint32_t>(ESP.getFreeHeap()), static_cast<uint32_t>(ESP.getMaxAllocHeap())};
}

// `id` and `path` come from the manifest index, whose fields are
// server-sanitised to exclude '"' and '\\' (crosspoint-sync docs/API.md:
// "path is already sanitised for FAT/exFAT -- no \ / : * ? \" < > |"), so
// they can be embedded in a JSON string literal directly, the same
// assumption ManifestIndexFormat.h relies on for its own '|' delimiter.
void logStage(const char* stage, const std::string& id, const std::string& path, const HeapPair& heap) {
  if (!Serial) return;
  logSerial.printf(
      "[TEST] {\"component\":\"download_queue\",\"stage\":\"%s\",\"id\":\"%s\",\"path\":\"%s\","
      "\"free\":%u,\"maxAlloc\":%u}\n",
      stage, id.c_str(), path.c_str(), (unsigned)heap.freeHeap, (unsigned)heap.maxAllocHeap);
}

void logItemEnd(const std::string& id, const std::string& path, const book_downloader::DownloadResult& result) {
  if (!Serial) return;
  logSerial.printf(
      "[TEST] {\"component\":\"download_queue\",\"stage\":\"item_end\",\"id\":\"%s\",\"path\":\"%s\","
      "\"ok\":%s,\"error\":\"%s\",\"httpStatus\":%d,\"bytesDownloaded\":%llu,\"free\":%u,\"maxAlloc\":%u}\n",
      id.c_str(), path.c_str(), result.ok ? "true" : "false", result.error.c_str(), result.httpStatus,
      (unsigned long long)result.bytesDownloaded, (unsigned)result.afterDownload.freeHeap,
      (unsigned)result.afterDownload.maxAllocHeap);
}

// Owns the queue's mutable state (QueueState) and the background worker
// task. A single instance at namespace scope, constructed at static-init
// time (its mutex created in the constructor, before setup() ever runs) --
// the exact pattern HalStorage::instance already uses for its own mutex, so
// this needs no lazy-init guard.
class Worker {
 public:
  Worker() : mutex_(xSemaphoreCreateMutex()) { assert(mutex_ != nullptr); }

  EnqueueOutcome enqueue(const std::string& id) {
    if (!SYNC_STORE.isPaired()) return EnqueueOutcome::NotPaired;
    ManifestIndexRecord record;
    if (!sync_manifest::findById(id, record)) return EnqueueOutcome::NotFound;

    QueueState::EnqueueResult result;
    {
      Lock lock(mutex_);
      result = state_.enqueue(id, record.path, record.sizeBytes);
      if (result == QueueState::EnqueueResult::Ok && taskHandle_ == nullptr) startLocked();
    }

    switch (result) {
      case QueueState::EnqueueResult::Ok:
        return EnqueueOutcome::Ok;
      case QueueState::EnqueueResult::Full:
        return EnqueueOutcome::Full;
      case QueueState::EnqueueResult::AlreadyQueued:
        return EnqueueOutcome::AlreadyQueued;
    }
    return EnqueueOutcome::Full;  // unreachable
  }

  void cancelAll() {
    Lock lock(mutex_);
    cancelRequested_ = true;
    state_.cancelAll();
  }

  Snapshot snapshot() {
    Lock lock(mutex_);
    Snapshot s;
    s.count = state_.copyItemsTo(s.items, MAX_QUEUE);
    s.workerRunning = taskHandle_ != nullptr;
    s.lastResult = state_.lastResult();
    return s;
  }

  // Both counters read under one lock acquisition: a caller comparing them
  // against its own cached pair must see a consistent pair, not one from
  // before a finish() and one from after.
  Pulse pulse() {
    Lock lock(mutex_);
    return Pulse{state_.generation(), state_.completions()};
  }

  void setSafetyCheck(SafetyCheck fn) {
    Lock lock(mutex_);
    safetyCheck_ = fn;
  }

 private:
  struct Lock {
    SemaphoreHandle_t handle;
    explicit Lock(SemaphoreHandle_t h) : handle(h) { xSemaphoreTake(handle, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(handle); }
  };

  struct ProgressCtx {
    Worker* self;
    std::string id;
  };

  // Caller must already hold mutex_.
  void startLocked() {
    cancelRequested_ = false;
    const BaseType_t created =
        xTaskCreate(&Worker::taskTrampoline, "DownloadQueue", WORKER_STACK_BYTES, this, 1, &taskHandle_);
    if (created != pdPASS) {
      LOG_ERR("DLQ", "Failed to create worker task");
      taskHandle_ = nullptr;
    }
  }

  static void taskTrampoline(void* param) { static_cast<Worker*>(param)->run(); }

  void run() {
    while (true) {
      std::string id, path;
      {
        Lock lock(mutex_);
        const QueueItem* front = state_.front();
        if (!front) {
          taskHandle_ = nullptr;
          break;  // Lock's destructor releases the mutex before the loop exits
        }
        if (!safetyCheck_ || safetyCheck_()) {
          id = front->id;
          path = front->path;
          state_.markDownloading(id);
          cancelRequested_ = false;
        } else {
          // Left empty -- paused, item stays Pending, checked again below. Only logged
          // reason available here: the check itself lives in main.cpp and doesn't say
          // which of its two conditions (library_sync active, or a book open) fired.
          LOG_DBG("DLQ", "Holding off %s: safety check refused (library_sync active or a book is open)",
                  front->id.c_str());
        }
      }

      if (id.empty()) {
        vTaskDelay(WORKER_IDLE_DELAY);
        continue;
      }

      logStage("item_start", id, path, sampleHeap());

      ProgressCtx pctx{this, id};
      const book_downloader::DownloadResult result =
          book_downloader::download(id, &Worker::onProgress, &pctx, &cancelRequested_);

      {
        Lock lock(mutex_);
        state_.finish(id, result.ok, result.error);
      }
      logItemEnd(id, path, result);
      // Bytes of stack never touched. If this approaches zero the task is one
      // library change away from the overflow that used to crash the device.
      if (Serial) {
        logSerial.printf("[TEST] {\"component\":\"download_queue\",\"stage\":\"stack\",\"headroomBytes\":%u}\n",
                         (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
      }

      vTaskDelay(1);  // yield between books rather than looping straight into the next one
    }
    vTaskDelete(nullptr);
  }

  static void onProgress(void* ctx, uint64_t downloaded, uint64_t total) {
    auto* p = static_cast<ProgressCtx*>(ctx);
    p->self->updateProgress(p->id, downloaded, total);
  }

  void updateProgress(const std::string& id, uint64_t downloaded, uint64_t total) {
    Lock lock(mutex_);
    state_.updateProgress(id, downloaded, total);
  }

  SemaphoreHandle_t mutex_ = nullptr;
  QueueState state_;
  TaskHandle_t taskHandle_ = nullptr;
  // Mirrors HttpDownloader::downloadToFile's own `bool* cancelFlag` contract
  // exactly (plain bool, not atomic/volatile) -- book_downloader::download()
  // only ever reads it between whole HTTP chunks, and a single-byte
  // read/write needs no extra synchronization on this ISA.
  bool cancelRequested_ = false;
  SafetyCheck safetyCheck_ = nullptr;
};

Worker gWorker;

}  // namespace

void setSafetyCheck(SafetyCheck fn) { gWorker.setSafetyCheck(fn); }

EnqueueOutcome enqueue(const std::string& id) { return gWorker.enqueue(id); }

void cancelAll() { gWorker.cancelAll(); }

Snapshot snapshot() { return gWorker.snapshot(); }

Pulse pulse() { return gWorker.pulse(); }

}  // namespace download_queue
