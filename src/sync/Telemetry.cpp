#include "Telemetry.h"

#include <ArduinoJson.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HeartbeatPayload.h>
#include <Logging.h>

#include <utility>

#include "SyncCredentialStore.h"
#include "network/HttpDownloader.h"

namespace telemetry {

namespace {

// outResponse, if non-null, receives the buffered response body -- only the
// heartbeat has anything to read out of it (see parseWallpaperRevision()).
TelemetryResult post(const std::string& path, const std::string& body, const uint32_t timeoutMs,
                     std::string* outResponse = nullptr) {
  TelemetryResult result;
  if (!SYNC_STORE.isPaired()) {
    result.error = "not_paired";
    return result;
  }

  const std::string url = SYNC_STORE.getBaseUrl() + path;
  std::string response;
  const bool ok =
      HttpDownloader::postJson(url, body, response, &result.httpStatus, SYNC_STORE.getAccessToken(), timeoutMs);

  // outStatus is left at its initial -1 only on a connect/TLS/DNS failure
  // that never got an HTTP response at all (see postJson's doc comment) --
  // check that first, since a real status code always takes precedence.
  // Any 2xx counts. These endpoints answer 201 when they create something --
  // request-books does exactly that -- and treating that as a failure told the
  // reader their request had not been sent when the server had already
  // recorded it, which invites them to press the button again.
  const bool accepted = result.httpStatus >= 200 && result.httpStatus < 300;
  if (result.httpStatus < 0) {
    result.error = "transport";
  } else if (!accepted) {
    result.error = "http_status";
  } else if (!ok) {
    result.error = "incomplete";  // accepted, but the body read didn't complete
  } else {
    result.ok = true;
  }
  if (!result.ok) {
    LOG_ERR("TELEM", "POST %s failed (ok=%d status=%d)", path.c_str(), ok, result.httpStatus);
  }
  if (outResponse) *outResponse = std::move(response);
  return result;
}

// The heartbeat response's opaque wallpaper-set fingerprint. Anything the
// contract does not guarantee -- absent, non-numeric, negative, zero, or
// wider than uint32 -- reads back as 0 ("unknown"), so a server that
// predates the field leaves wallpaper syncing exactly as it was.
uint32_t parseWallpaperRevision(const std::string& body) {
  if (body.empty()) return 0;
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return 0;
  // is<uint32_t>() is false for a string, a float, a negative and anything
  // that does not fit -- exactly the set that must read back as 0.
  JsonVariantConst revision = doc["wallpaperRevision"];
  if (!revision.is<uint32_t>()) return 0;
  return revision.as<uint32_t>();
}

}  // namespace

TelemetryResult sendHeartbeat(const HeartbeatInfo& info, const uint32_t timeoutMs) {
  JsonDocument doc;
  doc["firmwareVersion"] = CROSSPOINT_VERSION;
  if (info.batteryPercent >= 0) doc["batteryPercent"] = info.batteryPercent;
  if (info.sdTotalBytes != UINT64_MAX) doc["sdTotalBytes"] = info.sdTotalBytes;
  if (info.sdFreeBytes != UINT64_MAX) doc["sdFreeBytes"] = info.sdFreeBytes;
  if (!info.lastSyncStatus.empty()) doc["lastSyncStatus"] = info.lastSyncStatus;
  if (!info.lastErrorCode.empty()) doc["lastErrorCode"] = info.lastErrorCode;
  if (!info.lastErrorDetail.empty()) doc["lastErrorDetail"] = info.lastErrorDetail;

  std::string body;
  serializeJson(doc, body);
  std::string response;
  TelemetryResult result = post("/devices/heartbeat", body, timeoutMs, &response);
  // Only a 2xx body is worth reading: an error body carries no revision, and
  // leaving the field at 0 is what "unknown" already means.
  if (result.ok) result.wallpaperRevision = parseWallpaperRevision(response);
  return result;
}

HeartbeatInfo currentDeviceHeartbeatInfo() {
  HeartbeatInfo info;
  info.batteryPercent = powerManager.getBatteryPercentage();
  const uint64_t totalBytes = Storage.sdTotalBytes();
  info.sdTotalBytes = heartbeat_payload::computeSdTotalBytes(totalBytes);
  info.sdFreeBytes = heartbeat_payload::computeSdFreeBytes(totalBytes, Storage.sdUsedBytes());
  return info;
}

TelemetryResult requestBooks(const uint32_t timeoutMs) { return post("/feedback/request-books", "", timeoutMs); }

TelemetryResult bookFinished(const std::string& bookId, const std::string& documentHash, const uint32_t timeoutMs) {
  JsonDocument doc;
  if (!bookId.empty()) {
    doc["bookId"] = bookId;
  } else {
    doc["documentHash"] = documentHash;
  }
  std::string body;
  serializeJson(doc, body);
  return post("/events/book-finished", body, timeoutMs);
}

}  // namespace telemetry
