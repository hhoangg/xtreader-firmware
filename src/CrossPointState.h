#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

class CrossPointState : public PersistableStore<CrossPointState> {
  CrossPointState() = default;

  friend class PersistableStore<CrossPointState>;

 public:
  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  std::string openEpubPath;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};
  uint8_t recentSleepPos = 0;
  uint8_t recentSleepFill = 0;
  uint16_t recentOverlaySleepImages[SLEEP_RECENT_COUNT] = {};
  uint8_t recentOverlaySleepPos = 0;
  uint8_t recentOverlaySleepFill = 0;
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  bool showBootScreen = true;
  // Deferred "book finished" event: set by ReaderActivity the moment a book
  // reaches its end, cleared by HomeActivity once the event is actually
  // delivered (or found to have nothing to deliver) -- see
  // src/sync/BookFinishedNotifier.h. Persisted rather than a plain in-RAM
  // flag because this device reboots on every sleep-wake and the reader
  // never posts the event itself: doing so from the end-of-book screen would
  // risk a TLS session against the ~50 KB a reading session leaves, which
  // docs/API.md calls out as *not* comfortable (unlike the ~137 KB free at
  // the library screen, where this is actually sent). Empty means "no
  // pending event". Holds at most one book at a time -- finishing a second
  // book before the first is delivered (e.g. picking a suggested next book
  // straight from the end-of-book menu, bypassing Home) overwrites it; see
  // this task's report for why that bounded loss is an acceptable trade.
  std::string pendingBookFinishedPath;

  // Back-off state shared by enterDeepSleep()'s headless before-sleep Wi-Fi
  // attempt AND HomeActivity's once-per-boot library-screen Wi-Fi bring-up
  // (see src/sync/SleepProgressSync.h's loadWifiBackoffState()/
  // saveWifiBackoffState()) -- both are "is there Wi-Fi here" attempts
  // against the same saved credentials, so they share one counter rather
  // than each paying the back-off cost separately. See
  // lib/SyncManifest/SleepWifiBackoffPolicy.h for the schedule these two
  // fields drive. Persisted here (rather than a plain static) because deep
  // sleep is a full chip reset: nothing in RAM survives a wake, but this
  // file does. The sleep path's save is free (piggybacks on the
  // APP_STATE.saveToFile() call enterDeepSleep() already makes for
  // showBootScreen); the library-screen path's save is its own SD write,
  // but only when this state actually changes and at most once per boot.
  uint8_t sleepWifiConsecutiveFailures = 0;
  uint8_t sleepWifiSkipsRemaining = 0;

  // Boots that have reached the library screen since the last successful
  // lock-screen wallpaper sync (src/sync/WallpaperSync.h), driving the
  // cadence in lib/SyncManifest/SyncTriggerPolicy.h
  // (WALLPAPER_SYNC_BOOT_INTERVAL). A boot count rather than a timestamp
  // because this board has no RTC and sleep is a full power cut -- there is
  // no wall clock to schedule against, and a boot is the only tick that
  // survives one. Persisted for the same reason the two fields above are:
  // nothing in RAM outlives a wake. UINT16_MAX means "never synced", which
  // is also what a state.json written before this field existed reads back
  // as, so an already-paired reader syncs on its next boot rather than in
  // eight.
  uint16_t bootsSinceWallpaperSync = UINT16_MAX;

  static const char* getFilePath() { return "/.crosspoint/state.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;
  bool isRecentOverlaySleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
  void pushRecentOverlaySleep(uint16_t idx);
};

#define APP_STATE CrossPointState::getInstance()
