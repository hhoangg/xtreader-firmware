#pragma once
#include <HalStorage.h>

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
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "", int* outStatus = nullptr);

  /**
   * POST a small JSON body and buffer the (small) response into outResponse.
   * For short request/response exchanges -- a few hundred bytes, like the
   * device-pairing endpoints -- not for large bodies, which should use the
   * streaming fetchUrl() overload above instead. outStatus, if non-null,
   * receives the HTTP status code; left untouched on a connect/TLS/DNS
   * failure that never got a response at all.
   */
  static bool postJson(const std::string& url, const std::string& jsonBody, std::string& outResponse,
                       int* outStatus = nullptr);

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "");
};
