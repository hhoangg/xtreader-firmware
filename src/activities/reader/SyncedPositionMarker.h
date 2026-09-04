#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <optional>
#include <string>

// Remembers the last position this book was *confirmed sent* to the sync
// server, so that a failed upload is retried instead of being forgotten.
//
// Without this, EpubReaderActivity's sync baseline was captured at book load
// from wherever the book resumed, i.e. from local state rather than from what
// the server acknowledged. Deep sleep is a full chip reset, so the sequence
// was: push fails (no Wi-Fi, backed-off, captive portal) -> wake -> resume at
// the same page -> baseline := that page -> hasUnsyncedProgress() is false ->
// the position is never sent again unless the reader happens to turn a page.
// Same defect class as the declined-prompt bug RemoteProgressMarker.h fixes:
// the baseline has to reflect the server, not the device.
//
// Eight little-endian bytes (two int32s: spine index, then page number) in the
// book's own cache directory, beside remote.bin and progress.bin. Written only
// after KOReaderSyncClient reports OK -- never on open, never on a page turn --
// so this adds no SD traffic to the reading path. No atomic rename either: a
// missing, short, unreadable or torn file all read back as "nothing was ever
// pushed", which costs one extra push, never a wrong position.
namespace SyncedPositionMarker {

// The pair EpubReaderActivity::hasUnsyncedProgress() compares. Not a
// CrossPointPosition: this file must stay loadable by the host test suite,
// and these two ints are the whole of what the comparison needs.
struct Position {
  int spineIndex = -1;
  int pageNumber = -1;
};

// Everything the network half (src/sync/SleepProgressSync.cpp) needs to stamp
// the marker once it knows the upload succeeded, carried alongside the
// KOReaderProgress payload rather than inside it: that struct is the KOSync
// wire format and must not grow device-local bookkeeping. An empty cachePath
// means "no book to stamp", which is what a capture that produced nothing
// leaves behind.
struct Receipt {
  std::string cachePath;
  Position position;
};

inline std::string markerPath(const std::string& cachePath) { return cachePath + "/synced.bin"; }

// std::nullopt when nothing has ever been confirmed sent for this book, which
// is also how a missing, short, unreadable or torn file reads. Negative fields
// are treated the same way: they cannot come from save() below, so they are
// garbage, and a garbage baseline is worse than no baseline.
inline std::optional<Position> load(const std::string& cachePath) {
  HalFile f;
  if (!Storage.openFileForRead("SPM", markerPath(cachePath), f)) return std::nullopt;
  uint8_t data[8];
  if (f.read(data, sizeof(data)) != static_cast<int>(sizeof(data))) return std::nullopt;

  uint32_t fields[2] = {0, 0};
  for (int field = 0; field < 2; field++) {
    for (int i = 3; i >= 0; i--) {
      fields[field] = (fields[field] << 8) | data[field * 4 + i];
    }
  }

  Position position;
  position.spineIndex = static_cast<int>(static_cast<int32_t>(fields[0]));
  position.pageNumber = static_cast<int>(static_cast<int32_t>(fields[1]));
  if (position.spineIndex < 0 || position.pageNumber < 0) return std::nullopt;
  return position;
}

// Call only once an upload has actually been acknowledged. A negative field is
// refused rather than written: that is the "no position" sentinel the reader
// starts from, and storing it would read back as "nothing pushed" anyway.
inline bool save(const std::string& cachePath, const Position& position) {
  if (cachePath.empty() || position.spineIndex < 0 || position.pageNumber < 0) return false;

  const uint32_t fields[2] = {static_cast<uint32_t>(static_cast<int32_t>(position.spineIndex)),
                              static_cast<uint32_t>(static_cast<int32_t>(position.pageNumber))};
  uint8_t data[8];
  for (int field = 0; field < 2; field++) {
    for (int i = 0; i < 4; i++) {
      data[field * 4 + i] = static_cast<uint8_t>((fields[field] >> (8 * i)) & 0xFF);
    }
  }

  const std::string path = markerPath(cachePath);
  HalFile f;
  if (!Storage.openFileForWrite("SPM", path, f)) {
    LOG_ERR("SPM", "Could not open %s for write", path.c_str());
    return false;
  }
  if (f.write(data, sizeof(data)) != sizeof(data)) {
    LOG_ERR("SPM", "Short write to %s", path.c_str());
    return false;
  }
  f.flush();
  return true;
}

// The baseline EpubReaderActivity::hasUnsyncedProgress() measures against,
// chosen at book load.
//
// DO NOT "simplify" the marker-absent case into something that always reports
// unsynced. A book that has never been pushed would then upload its resume
// position on every open-and-sleep, including when the reader did not read a
// word -- and that stale local position would overwrite a newer one another
// device had already pushed. Falling back to the current position means a
// never-pushed book behaves exactly as it did before this marker existed:
// only real movement counts as unsynced.
inline Position baselineFor(const std::optional<Position>& marker, const Position& current) {
  return marker ? *marker : current;
}

// The whole decision, as a pure function: `atLoad` is where the book resumed,
// `now` is where the reader actually is when sleep fires. The case this
// exists to get right is marker-present-and-older-than-atLoad, i.e. the last
// push failed -- then `now` differs from the baseline even if the reader never
// turned a page, and the next sleep retries.
inline bool positionNeedsPush(const std::optional<Position>& marker, const Position& atLoad, const Position& now) {
  const Position baseline = baselineFor(marker, atLoad);
  return now.spineIndex != baseline.spineIndex || now.pageNumber != baseline.pageNumber;
}

}  // namespace SyncedPositionMarker
