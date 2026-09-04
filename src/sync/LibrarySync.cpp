#include "LibrarySync.h"

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cassert>

#include "SleepProgressSync.h"
#include "SleepWifiBackoffPolicy.h"
#include "SyncCredentialStore.h"
#include "SyncManifest.h"
#include "SyncTriggerPolicy.h"
#include "Telemetry.h"
#include "activities/RenderLock.h"

namespace library_sync {

namespace {

// A wolfSSL handshake plus the HTTP and SD layers above it overflows the
// 4096 bytes CLAUDE.md's table suggests for network work: on hardware that
// crashed the device outright, at the first byte of the first network read
// (see DownloadQueue.cpp). DownloadQueue.cpp and RemoteProgressCheck.cpp
// both measured 12288 as the number that survives this exact TLS+HTTP+SD
// combination; this task reuses that number rather than re-measuring it.
// Task 4 measures this worker's own high-water mark on hardware -- until
// then this is the only defensible value.
constexpr uint32_t WORKER_STACK_BYTES = 12288;

// Owns the worker's mutable state and its background task. A single
// instance at namespace scope, constructed at static-init time (its mutex
// created in the constructor, before setup() ever runs) -- the same pattern
// download_queue::gWorker and remote_progress::checker() already use.
class Worker {
 public:
  Worker() : mutex_(xSemaphoreCreateMutex()) { assert(mutex_ != nullptr); }

  void setSafetyCheck(SafetyCheck fn) {
    Lock lock(mutex_);
    safetyCheck_ = fn;
  }

  bool start() {
    Lock lock(mutex_);
    if (taskHandle_ != nullptr) return false;  // already running
    // HomeActivity calls start() on every repaint (cursor movement, download
    // queue changes, a returning activity), by design -- polling is cheaper
    // than wiring a push path through an Activity this module must not know
    // about. That means the common case, after the one real sync per boot,
    // is "nothing to do", and it has to be the cheap path: without this
    // check every one of those repaints would xTaskCreate() a 12288-byte
    // stack only for the task to wake up, find both gates below permanently
    // closed, and self-delete -- heap churn on a device whose largest free
    // block already falls to ~7 KB mid-download, and outright xTaskCreate
    // failure if that block isn't there. Reuses run()'s own gates (rather
    // than re-deriving "is there work" here) so the two can never disagree;
    // WiFi.status() and SYNC_STORE.isPaired() are both plain in-memory
    // reads, safe to call from the render task on every repaint.
    if (!hasWorkToDo()) return false;
    if (safetyCheck_ && !safetyCheck_()) return false;

    const BaseType_t created =
        xTaskCreate(&Worker::taskTrampoline, "LibrarySync", WORKER_STACK_BYTES, this, 1, &taskHandle_);
    if (created != pdPASS) {
      LOG_ERR("LIBSYNC", "Failed to create worker task");
      taskHandle_ = nullptr;
      return false;
    }
    return true;
  }

  Status status() {
    Lock lock(mutex_);
    return status_;
  }

 private:
  struct Lock {
    SemaphoreHandle_t handle;
    explicit Lock(SemaphoreHandle_t h) : handle(h) { xSemaphoreTake(handle, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(handle); }
  };

  // run() always returns through finish() (which clears taskHandle_ under
  // the lock) before getting here, so it is always safe to delete the
  // calling task immediately afterward -- a FreeRTOS task function must
  // never simply return, unlike an ordinary C++ function.
  static void taskTrampoline(void* param) {
    static_cast<Worker*>(param)->run();
    vTaskDelete(nullptr);
  }

  // Phase and generation always move together under one lock acquisition,
  // so a poller reading status() never pairs the phase from one transition
  // with the generation of another.
  void setPhase(Phase phase) {
    Lock lock(mutex_);
    status_.phase = phase;
    status_.generation++;
  }

  // Every exit from run() goes through here: drops back to Idle, bumps
  // generation, and -- only when a sync actually ran -- records whether it
  // succeeded. Early-outs (backed off, no Wi-Fi, gated out) pass
  // syncRan=false, which leaves lastSyncRan/lastSyncOk exactly as they were,
  // since neither field describes this attempt.
  void finish(bool syncRan, bool syncOk) {
    Lock lock(mutex_);
    status_.phase = Phase::Idle;
    status_.generation++;
    if (syncRan) {
      status_.lastSyncRan = true;
      status_.lastSyncOk = syncOk;
    }
    taskHandle_ = nullptr;
  }

  // Matches HomeActivity.cpp's original, unconditional
  // `heartbeatWallpaperRevision = heartbeatResult.wallpaperRevision;` -- not
  // gated on heartbeatResult.ok, because TelemetryResult::wallpaperRevision
  // is already 0 on any failure (absent field, transport error, or HTTP
  // error), which is exactly the "leave the wallpaper cadence to it" value.
  void setWallpaperRevision(uint32_t revision) {
    Lock lock(mutex_);
    status_.wallpaperRevision = revision;
  }

  // sleep_progress_sync::saveWifiBackoffState() can write CrossPointState to
  // SD (CrossPointState::saveToFile()), which shares the SD card's SPI bus
  // with the display. HomeActivity's original call site never needed this
  // lock because it ran on the render task, which already holds
  // ActivityManager's rendering mutex for the whole render() call; this
  // worker runs on its own task and must take that lock explicitly around
  // the write -- the same thing RemoteProgressCheck.cpp's own
  // saveBackoffState() helper does for the identical call.
  static void saveBackoffState(const sleep_wifi_backoff::State& state) {
    RenderLock lock;
    sleep_progress_sync::saveWifiBackoffState(state);
  }

  // True if run() could still do something this boot. Once
  // wifiConnectAttemptedThisBoot_ is set, shouldAttemptLibraryWifiConnect()
  // is permanently false for the rest of the boot regardless of paired/
  // wifiConnected; once manifestSyncAttemptedThisBoot_ is set, the same is
  // true of shouldAutoSync(). So calling the actual predicates here (instead
  // of just testing the two latches) is exactly equivalent, costs two cheap
  // in-memory reads, and can never drift from run()'s own gates if either
  // predicate's inputs ever grow beyond these three.
  bool hasWorkToDo() const {
    const bool paired = SYNC_STORE.isPaired();
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    return sync_trigger::shouldAttemptLibraryWifiConnect(paired, wifiConnected, wifiConnectAttemptedThisBoot_) ||
           sync_trigger::shouldAutoSync(paired, wifiConnected, manifestSyncAttemptedThisBoot_);
  }

  void run() {
    bool wifiConnected = WiFi.status() == WL_CONNECTED;
    const bool paired = SYNC_STORE.isPaired();
    // Only set once this run itself brings WiFi up (see the gate below,
    // which requires wifiConnected == false to even enter that branch) --
    // WiFi that was already up for some unrelated reason must not touch the
    // back-off counter shared with the sleep path.
    bool broughtWifiUp = false;
    sleep_wifi_backoff::State backoffState;

    // The bring-up itself: once per boot, only when paired and not already
    // connected (see SyncTriggerPolicy.h's shouldAttemptLibraryWifiConnect()
    // for why this is now worth doing -- nothing else in a production build
    // ever connects WiFi, so without this the automatic sync below never
    // runs at all).
    if (sync_trigger::shouldAttemptLibraryWifiConnect(paired, wifiConnected, wifiConnectAttemptedThisBoot_)) {
      // Locked even though this task is the only writer: start() reads both
      // latches from the render task via hasWorkToDo(), and this firmware
      // also ships as sticky-gh_release on the dual-core ESP32-S3, where a
      // locked read against an unlocked write on the other core has no
      // ordering guarantee.
      {
        Lock lock(mutex_);
        wifiConnectAttemptedThisBoot_ = true;
      }

      backoffState = sleep_progress_sync::loadWifiBackoffState();
      if (!sleep_wifi_backoff::shouldAttempt(backoffState)) {
        LOG_DBG("LIBSYNC",
                "Skipping library WiFi bring-up: backed off (%u skip(s) left after %u consecutive failure(s))",
                backoffState.skipsRemaining, backoffState.consecutiveFailures);
        saveBackoffState(sleep_wifi_backoff::afterSkippedAttempt(backoffState));
        return finish(/*syncRan=*/false, /*syncOk=*/false);
      }

      setPhase(Phase::ConnectingWifi);
      bool cancelled = false;
      // callerHoldsRenderLock=false: this runs on its own task, holding no
      // render lock -- unlike the render-task caller this replaces, which
      // had to pass true or deadlock itself (renderingMutex is not
      // recursive). false makes connectToSavedWifi() take that lock itself
      // around its WifiCredentialStore SD read, exactly like
      // RemoteProgressCheck.cpp's identical background-task call.
      // pollPowerButton=false: gpio.update() may only be called from the
      // loop task, the one that owns MappedInputManager/HalGPIO -- calling
      // it from any other task recomputes the button edge words underneath
      // the owner and can silently swallow a page turn (see
      // connectToSavedWifi()'s header comment). This worker draws nothing,
      // so there is no visible bring-up for a power-button press to
      // interrupt; it stays bounded by WIFI_CONNECT_TIMEOUT_MS alone, same
      // as RemoteProgressCheck.cpp's worker. cancelled can therefore never
      // come back true and is not checked, matching that same call site.
      wifiConnected = sleep_progress_sync::connectToSavedWifi(cancelled, /*callerHoldsRenderLock=*/false,
                                                              /*pollPowerButton=*/false);

      if (!wifiConnected) {
        // No network reached at all -- back off exactly as the sleep path
        // does when the search itself finds nothing (see
        // SleepWifiBackoffPolicy.h). shouldAutoSync requires wifiConnected,
        // so there is nothing left to do this pass and no second, more
        // precise "did we reach the real internet" signal coming for this
        // attempt.
        saveBackoffState(sleep_wifi_backoff::afterAttempt(backoffState, false));
        return finish(/*syncRan=*/false, /*syncOk=*/false);
      }
      broughtWifiUp = true;
    }

    if (!sync_trigger::shouldAutoSync(paired, wifiConnected, manifestSyncAttemptedThisBoot_)) {
      return finish(/*syncRan=*/false, /*syncOk=*/false);
    }
    {
      Lock lock(mutex_);
      manifestSyncAttemptedThisBoot_ = true;
    }

    setPhase(Phase::Syncing);
    // Bounded, but with its own budget rather than the shorter power-off one
    // -- see SyncTriggerPolicy.h's HOME_SYNC_TIMEOUT_MS for why the two
    // differ. A captive portal or black-holed server still must not stall
    // this worker indefinitely.
    const sync_manifest::SyncResult syncResult = sync_manifest::sync(sync_trigger::HOME_SYNC_TIMEOUT_MS);

    if (broughtWifiUp) {
      // The manifest fetch above is the first real proof this bring-up
      // reached more than just the access point -- a captive portal
      // associates too, then this fetch fails exactly like "no Wi-Fi here"
      // (see SleepWifiBackoffPolicy.h's reachedNetwork() for the same
      // reasoning on the sleep path, and SyncManifest.cpp for where
      // "fetch_failed" is set). Any other error (not_paired can't happen
      // here -- paired was already checked above; sd_write_failed,
      // corrupt_index, missing_trailer, too_many_pages, rename_failed)
      // still proves a real response came back, so it must not count as "no
      // Wi-Fi here" either.
      const bool reached = syncResult.error != "fetch_failed";
      saveBackoffState(sleep_wifi_backoff::afterAttempt(backoffState, reached));
    }

    // WiFi is already up for the manifest sync above -- one of the two
    // moments (task brief) a heartbeat can ride along without paying its own
    // WiFi cost. Best-effort: a failed heartbeat must not affect the library
    // sync it rides with, so its result is only logged here, never surfaced
    // through Status.
    telemetry::HeartbeatInfo heartbeatInfo = telemetry::currentDeviceHeartbeatInfo();
    heartbeatInfo.lastSyncStatus = syncResult.ok ? "ok" : "failed";
    const telemetry::TelemetryResult heartbeatResult =
        telemetry::sendHeartbeat(heartbeatInfo, sync_trigger::AUTO_SYNC_TIMEOUT_MS);
    if (!heartbeatResult.ok) {
      LOG_DBG("LIBSYNC", "Heartbeat piggybacked on library sync failed (error=%s status=%d) -- diagnostics only",
              heartbeatResult.error.c_str(), heartbeatResult.httpStatus);
    }
    // The one thing the heartbeat brings back that changes behaviour: how
    // trySyncWallpapers() learns the assigned set was edited without waiting
    // out the boot cadence -- see Status::wallpaperRevision's comment.
    setWallpaperRevision(heartbeatResult.wallpaperRevision);

    LOG_DBG("LIBSYNC", "Worker stack high water: %u bytes",
            (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    finish(/*syncRan=*/true, syncResult.ok);
  }

  SemaphoreHandle_t mutex_ = nullptr;
  TaskHandle_t taskHandle_ = nullptr;
  SafetyCheck safetyCheck_ = nullptr;
  Status status_;

  // Once-per-boot latches, mirroring HomeActivity's own file-scope statics
  // before this task existed: this Worker is a namespace-scope singleton
  // that survives every HomeActivity instance (HomeActivity is destroyed and
  // recreated on every Home visit), constructed once at static-init and
  // reset only by a real reboot -- which this device also goes through on
  // every sleep wake, so "once per boot" and "once per wake" are the same
  // event here.
  bool wifiConnectAttemptedThisBoot_ = false;
  bool manifestSyncAttemptedThisBoot_ = false;
};

Worker gWorker;

}  // namespace

void setSafetyCheck(SafetyCheck fn) { gWorker.setSafetyCheck(fn); }

bool start() { return gWorker.start(); }

Status status() { return gWorker.status(); }

}  // namespace library_sync
