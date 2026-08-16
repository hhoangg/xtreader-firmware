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

  static const char* getFilePath() { return "/.crosspoint/state.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;
  bool isRecentOverlaySleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
  void pushRecentOverlaySleep(uint16_t idx);
};

#define APP_STATE CrossPointState::getInstance()
