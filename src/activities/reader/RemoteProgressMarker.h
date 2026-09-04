#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <string>

// Remembers which remote position this book has already been asked about, so
// that answering the "continue from another device?" dialog once settles it.
// Without this, declining is forgotten the moment the reader closes the book
// and the same row asks again on the next open, forever.
//
// The key is the server's own `timestamp` for the row: the device has no
// clock, so the value is stored and compared, never interpreted. A genuinely
// newer position from the other device carries a larger timestamp and still
// prompts -- see remote_progress_policy::decide().
//
// Eight little-endian bytes in the book's own cache directory, beside
// progress.bin. Written only when the reader answers a dialog -- never on
// open, never on a page turn -- so this adds no SD traffic to the reading
// path. No atomic rename either: a torn write reads back as "never
// resolved", which costs one extra prompt, not a wrong position.
namespace RemoteProgressMarker {

inline std::string markerPath(const std::string& cachePath) { return cachePath + "/remote.bin"; }

// 0 when this book has never been resolved, which is also how a missing,
// short or unreadable file reads.
inline int64_t load(const std::string& cachePath) {
  HalFile f;
  if (!Storage.openFileForRead("RPM", markerPath(cachePath), f)) return 0;
  uint8_t data[8];
  if (f.read(data, sizeof(data)) != static_cast<int>(sizeof(data))) return 0;
  uint64_t value = 0;
  for (int i = 7; i >= 0; i--) {
    value = (value << 8) | data[i];
  }
  return static_cast<int64_t>(value);
}

// No-op for an unstamped row: zero is what "never resolved" already means,
// so there would be nothing to compare it against later.
inline bool save(const std::string& cachePath, const int64_t timestamp) {
  if (timestamp <= 0) return false;
  const auto value = static_cast<uint64_t>(timestamp);
  uint8_t data[8];
  for (int i = 0; i < 8; i++) {
    data[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
  }

  const std::string path = markerPath(cachePath);
  HalFile f;
  if (!Storage.openFileForWrite("RPM", path, f)) {
    LOG_ERR("RPM", "Could not open %s for write", path.c_str());
    return false;
  }
  if (f.write(data, sizeof(data)) != sizeof(data)) {
    LOG_ERR("RPM", "Short write to %s", path.c_str());
    return false;
  }
  f.flush();
  return true;
}

}  // namespace RemoteProgressMarker
