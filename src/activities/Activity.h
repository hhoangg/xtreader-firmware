#pragma once
#include <Logging.h>

#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include "ActivityManager.h"  // for using the ActivityManager singleton
#include "ActivityResult.h"
#include "GfxRenderer.h"
#include "KOReaderSyncClient.h"  // for KOReaderProgress, captureProgressForSleep()'s out-param
#include "MappedInputManager.h"
#include "RenderLock.h"
#include "reader/SyncedPositionMarker.h"  // for captureProgressForSleep()'s second out-param
#include "util/ScreenshotInfo.h"

class Activity {
  friend class ActivityManager;

 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

  ActivityResultHandler resultHandler;
  ActivityResult result;

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}
  virtual ~Activity() = default;
  virtual void onEnter();
  virtual void onExit();
  virtual void loop() {}

  virtual void render(RenderLock&&) {}

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  virtual void requestUpdate(bool immediate = false);

  // Request an immediate render and block until it completes.
  virtual void requestUpdateAndWait();

  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  // Exclusive storage activities suspend global controls and normal activity
  // transitions so no filesystem code races a raw SD-card owner.
  virtual bool requiresExclusiveStorageLoop() const { return false; }
  virtual bool isReaderActivity() const { return false; }
  // Returns true when the activity schedules its own forced refresh.
  virtual bool handleForcedRefresh() { return false; }
  virtual bool isHomeActivity() const { return false; }
  virtual bool handleHomeGesture() { return false; }
  virtual ScreenshotInfo getScreenshotInfo() const { return {}; }
  // True when this activity has reading progress that has not been sent to a
  // sync server yet. Used only to decide whether enterDeepSleep() (main.cpp)
  // should run its headless before-sleep KOSync upload -- see
  // src/sync/SleepProgressSync.h and lib/SyncManifest/SyncTriggerPolicy.h's
  // shouldSyncBeforeSleep(). Default false; only EpubReaderActivity overrides.
  virtual bool hasUnsyncedProgress() const { return false; }
  // Headless equivalent of a manual "Sync Progress", split from the network
  // half so main.cpp's enterDeepSleep() can call this *while the activity is
  // still alive* -- ActivityManager::goToSleep() (called right after, to
  // paint the sleep screen as early as possible) destroys this activity, so
  // anything the upload needs must be captured first. Builds the current
  // position into outProgress and persists it to disk; does not touch the
  // network or WiFi. Returns true only when a payload was actually produced
  // (nothing to send, or no credentials, leaves outProgress untouched).
  // `outReceipt` carries what the upload's success must be recorded against
  // (the book's cache directory and the position being sent) so that a push
  // that fails is retried instead of forgotten -- see
  // reader/SyncedPositionMarker.h. It travels beside outProgress rather than
  // inside it because KOReaderProgress is the KOSync wire payload and must
  // not grow device-local bookkeeping.
  // Default no-op; only EpubReaderActivity overrides.
  virtual bool captureProgressForSleep(KOReaderProgress& outProgress, SyncedPositionMarker::Receipt& outReceipt) {
    return false;
  }
#ifdef CP_TEST_CONSOLE
  // Test-console introspection (CMD:ACTIVITY): the cheapest possible
  // assertion that navigation landed where it should.
  const std::string& getName() const { return name; }
  // Test-console introspection (CMD:SELECTED): reports whatever row/icon is
  // currently highlighted, so a host script can drive menu navigation by
  // reading real UI content instead of counting rows (a hardcoded row index
  // silently picks the wrong item once a conditional row shifts everything
  // after it; probing
  // by pressing CONFIRM and backing out is worse still, since it mutates
  // toggle settings in place and triggers real side effects like a Wi-Fi
  // scan on whatever it CONFIRMs along the way).
  //
  // Returns false when this activity doesn't support the introspection (not
  // a list/menu screen, or not wired up for it yet -- only the activities
  // scripts/device_tests/test_pairing.py actually drives implement this so
  // far). When it returns true, outIndex/outCount are always valid, but
  // outLabel may be empty if the current selection has no text label (e.g.
  // one of HomeActivity's recent-book cover tiles) -- callers should treat
  // that as "keep moving", not as an error.
  virtual bool getSelectedRowInfo(std::string& outLabel, int& outIndex, int& outCount) const { return false; }
#endif

  // Start a new activity without destroying the current one
  // Note: requestUpdate() will be invoked automatically once resultHandler finishes
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler);

  // Set the result to be passed back to the previous activity when this activity finishes
  void setResult(ActivityResult&& result);

  // Finish this activity and return to the previous one on the stack (if any)
  void finish();

  // Convenience method to facilitate API transition to ActivityManager
  // TODO: remove this in near future
  void onGoHome(HomeMenuItem item = HomeMenuItem::NONE);
  void onSelectBook(const std::string& path);
};
