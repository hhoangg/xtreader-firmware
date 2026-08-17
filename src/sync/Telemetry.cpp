#include "Telemetry.h"

#include <ArduinoJson.h>
#include <Logging.h>

#include "SyncCredentialStore.h"
#include "network/HttpDownloader.h"

namespace telemetry {

namespace {

TelemetryResult post(const std::string& path, const std::string& body) {
  TelemetryResult result;
  if (!SYNC_STORE.isPaired()) {
    result.error = "not_paired";
    return result;
  }

  const std::string url = SYNC_STORE.getBaseUrl() + path;
  std::string response;
  const bool ok = HttpDownloader::postJson(url, body, response, &result.httpStatus, SYNC_STORE.getAccessToken());

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
  return result;
}

}  // namespace

TelemetryResult sendHeartbeat(const HeartbeatInfo& info) {
  JsonDocument doc;
  doc["firmwareVersion"] = CROSSPOINT_VERSION;
  if (info.batteryPercent >= 0) doc["batteryPercent"] = info.batteryPercent;
  if (info.sdFreeBytes != UINT64_MAX) doc["sdFreeBytes"] = info.sdFreeBytes;
  if (!info.lastSyncStatus.empty()) doc["lastSyncStatus"] = info.lastSyncStatus;
  if (!info.lastErrorCode.empty()) doc["lastErrorCode"] = info.lastErrorCode;
  if (!info.lastErrorDetail.empty()) doc["lastErrorDetail"] = info.lastErrorDetail;

  std::string body;
  serializeJson(doc, body);
  return post("/devices/heartbeat", body);
}

TelemetryResult requestBooks() { return post("/feedback/request-books", ""); }

TelemetryResult bookFinished(const std::string& bookId, const std::string& documentHash) {
  JsonDocument doc;
  if (!bookId.empty()) {
    doc["bookId"] = bookId;
  } else {
    doc["documentHash"] = documentHash;
  }
  std::string body;
  serializeJson(doc, body);
  return post("/events/book-finished", body);
}

}  // namespace telemetry
