#pragma once

#include <cstdint>
#include <string>

/**
 * The three device-auth POST endpoints under "Feedback and telemetry" in
 * crosspoint-sync docs/API.md: heartbeat health data, "I want more books",
 * and "I finished this book". None of these stream or page -- each is one
 * small JSON POST via HttpDownloader::postJson, same shape as
 * SyncPairingActivity's own calls to /device/code and /device/token, just
 * with a bearer token attached.
 */
namespace telemetry {

struct TelemetryResult {
  bool ok = false;
  // Empty on success. One of: "not_paired", "transport" (no HTTP response
  // at all), "http_status" (see httpStatus).
  std::string error;
  int httpStatus = -1;
};

// POST /devices/heartbeat. Every field is optional server-side except the
// firmware version, which this always sends; an omitted field leaves the
// server's stored value alone rather than clearing it. Sentinels mark
// "omit": batteryPercent < 0, sdTotalBytes/sdFreeBytes == UINT64_MAX, and
// empty strings for the three std::string fields.
struct HeartbeatInfo {
  int batteryPercent = -1;
  // Card capacity -- lets the server (and the dashboard's storage bar) turn
  // sdFreeBytes into a fraction. Without it, sdFreeBytes alone has no
  // denominator.
  uint64_t sdTotalBytes = UINT64_MAX;
  uint64_t sdFreeBytes = UINT64_MAX;
  // One of "ok", "failed", "never" -- crosspoint-sync's HeartbeatRequest.lastSyncStatus enum
  // (packages/contract/src/telemetry.ts). Not free text: any other value fails server-side
  // validation and the whole heartbeat is rejected (400), including every other field in it.
  std::string lastSyncStatus;
  std::string lastErrorCode;    // the short on-screen code, e.g. "E-03"
  std::string lastErrorDetail;  // never shown on-device; sent here instead
};
TelemetryResult sendHeartbeat(const HeartbeatInfo& info);

// Fills batteryPercent, sdTotalBytes and sdFreeBytes from the live HAL
// (HalPowerManager::getBatteryPercentage(), HalStorage::sdTotalBytes()/
// sdUsedBytes()) -- the fields every heartbeat call site needs regardless of
// which sync it rides along with. lastSyncStatus/lastErrorCode/
// lastErrorDetail are left at their "omit" defaults for the caller to fill
// in with whatever it actually knows about the sync that brought WiFi up.
HeartbeatInfo currentDeviceHeartbeatInfo();

// POST /feedback/request-books -- no body. One button press: "I want more
// books" -- the product's whole point for someone who cannot add books
// themselves and cannot describe a fault (see docs/API.md's "Feedback and
// telemetry" section).
TelemetryResult requestBooks();

// POST /events/book-finished. Exactly one of bookId/documentHash should be
// non-empty; bookId takes precedence if both are (matching the API's `{
// "bookId" }` or `{ "documentHash" }` -- never both).
TelemetryResult bookFinished(const std::string& bookId, const std::string& documentHash = "");

}  // namespace telemetry
