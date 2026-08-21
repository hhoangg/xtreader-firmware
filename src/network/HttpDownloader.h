#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <functional>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files. Built on
 * esp_http_client: https is verified against the CA bundle, plain http is
 * used for local servers (transport is chosen from the URL scheme).
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  // Per-socket-op deadline used by every call below that doesn't pass its own
  // timeoutMs -- tuned for book and firmware downloads, which can be slow to
  // send headers or stall mid-body on a slow CDN. crosspoint-sync's
  // own small JSON/manifest endpoints pass a much shorter deadline instead
  // (see lib/SyncManifest/SyncTriggerPolicy.h's AUTO_SYNC_TIMEOUT_MS /
  // EXPLICIT_SYNC_TIMEOUT_MS) so a captive portal or black-holed server can't
  // stall a sync behind this download-tuned default.
  static constexpr uint32_t DEFAULT_TIMEOUT_MS = 60000;

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "");

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   * outStatus, if non-null, receives the final HTTP status code seen (the
   * last redirect hop's, or the terminal response's); left untouched on a
   * connect/TLS/DNS failure that never got a response at all.
   *
   * bearerToken, if non-empty, sends "Authorization: Bearer <token>" and
   * takes precedence over username/password Basic auth -- crosspoint-sync's
   * device-authenticated routes (GET /library/manifest, etc.) use an opaque
   * bearer token (see SyncCredentialStore), not Basic auth.
   *
   * timeoutMs overrides DEFAULT_TIMEOUT_MS for this request.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "", int* outStatus = nullptr, const std::string& bearerToken = "",
                       uint32_t timeoutMs = DEFAULT_TIMEOUT_MS);

  /**
   * POST a small JSON body and buffer the (small) response into outResponse.
   * For short request/response exchanges -- a few hundred bytes, like the
   * device-pairing endpoints -- not for large bodies, which should use the
   * streaming fetchUrl() overload above instead. outStatus, if non-null,
   * receives the HTTP status code; left untouched on a connect/TLS/DNS
   * failure that never got a response at all.
   *
   * bearerToken, if non-empty, sends "Authorization: Bearer <token>" -- the
   * crosspoint-sync telemetry/feedback routes (/devices/heartbeat,
   * /feedback/request-books, /events/*) are device-authenticated the same
   * way GET /library/manifest is (see the streaming fetchUrl() overload's
   * comment). Device pairing's own POSTs (/device/code, /device/token) take
   * no auth at all, so they simply omit this argument.
   *
   * timeoutMs overrides DEFAULT_TIMEOUT_MS for this request.
   */
  static bool postJson(const std::string& url, const std::string& jsonBody, std::string& outResponse,
                       int* outStatus = nullptr, const std::string& bearerToken = "",
                       uint32_t timeoutMs = DEFAULT_TIMEOUT_MS);

  /**
   * Send an HTTP DELETE with no body, buffering the (small) response into
   * outResponse -- the crosspoint-sync "force delete" route
   * (DELETE /library/:id) is the only caller today. outStatus, if non-null,
   * receives the HTTP status code; left untouched on a connect/TLS/DNS
   * failure that never got a response at all, same convention as postJson.
   * bearerToken, if non-empty, sends "Authorization: Bearer <token>".
   */
  static bool deleteResource(const std::string& url, std::string& outResponse, int* outStatus = nullptr,
                             const std::string& bearerToken = "");

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "");
};
