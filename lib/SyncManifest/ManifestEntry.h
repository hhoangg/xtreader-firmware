#pragma once

#include <cstdint>
#include <string>

// One row of GET /library/manifest's newline-delimited response body -- see
// crosspoint-sync docs/API.md, "GET /library/manifest":
//   {"id":"bok_...","path":"/Kỹ năng/Đắc Nhân Tâm.epub","sizeBytes":3014656,
//    "contentHash":"...","updatedAt":1755300000}
//
// std::string fields, not fixed char buffers (contrast
// lib/DevicePairing/DevicePairingProtocol.h's DeviceCodeResponse): those
// buffers exist because their contents get copied straight into
// SyncCredentialStore's fixed NVS-backed fields. A ManifestEntry instead
// gets formatted straight into one index-file line and then discarded --
// see ManifestStreamParser.h's class comment -- so there is exactly one
// alive at a time regardless of library size, and a fixed cap here would
// only risk silently truncating a real (possibly long, Vietnamese) title.
struct ManifestEntry {
  std::string id;
  std::string path;
  uint64_t sizeBytes = 0;
  std::string contentHash;
  // Opaque server timestamp (unix seconds). The device has no RTC (see
  // docs/API.md's constraints section) -- store and compare, never interpret
  // as wall-clock time.
  uint64_t updatedAt = 0;
  // Only set on a `since` delta response; a full sync (no `since`, what this
  // firmware currently issues -- see SyncManifest.cpp) never sends these.
  // Parsed anyway so the wire format is handled correctly the day delta
  // sync ships; see ManifestIndex.h for how the index currently reacts to one.
  bool deleted = false;
};

// The trailer line every manifest page ends with:
//   {"done":true,"nextCursor":"...","totalCount":842}
struct ManifestTrailer {
  bool done = false;
  std::string nextCursor;  // empty when hasNextCursor is false (JSON null / absent)
  bool hasNextCursor = false;
  uint32_t totalCount = 0;
};
