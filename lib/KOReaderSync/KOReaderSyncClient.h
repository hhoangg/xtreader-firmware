#pragma once
#include <cstdint>
#include <optional>
#include <string>

/**
 * Optional document metadata sent alongside progress sync requests.
 * Mirrors the metadata object added in KOReader PR #15306.
 * The official sync server ignores this field; custom servers may use it.
 */
struct KOReaderMetadata {
  std::string filename;  // e.g. "my_book.epub"
  std::string title;     // Document title from EPUB metadata
  std::string authors;   // Author(s) from EPUB metadata
};

/**
 * Rich CrossPoint position sent alongside progress uploads. Maps 1:1 onto the
 * crosspoint-sync extended `position` object (see crosspoint-sync docs/API.md).
 * It is only transmitted to sync.crosspointreader.com. These fields remain
 * layout-dependent compatibility hints; the standard XPath is the content anchor.
 */
struct KOReaderRichPosition {
  uint32_t pctQ = 0;                       // Percentage quantized 0..1,000,000 (metadata/fallback)
  uint16_t spineIndex = 0;                 // Spine (chapter) index
  uint16_t pageNumber = 0;                 // Page within spine (layout-dependent hint)
  uint16_t totalPages = 1;                 // Spine page count (layout-dependent hint)
  std::optional<uint16_t> paragraphIndex;  // Synthetic 1-based paragraph index
  std::string xpath;                       // KOReader-style xpath (server cap: 120 bytes)
};

/**
 * Progress data from KOReader sync server.
 */
struct KOReaderProgress {
  std::string document;                          // Document hash
  std::string progress;                          // XPath-like progress string
  float percentage;                              // Progress percentage (0.0 to 1.0)
  std::string device;                            // Device name
  std::string deviceId;                          // Device ID
  int64_t timestamp;                             // Unix timestamp of last update
  std::optional<KOReaderMetadata> metadata;      // Optional document metadata
  std::optional<KOReaderRichPosition> position;  // Optional rich position (crosspoint-sync servers only)
};

/**
 * HTTP client for KOReader sync API.
 *
 * Base URL: https://sync.koreader.rocks:443/
 *
 * API Endpoints:
 *   GET /users/auth - Authenticate (validate credentials)
 *   GET /syncs/progress/:document - Get progress for a document
 *   PUT /syncs/progress - Update progress for a document
 *
 * Authentication:
 *   x-auth-user: username
 *   x-auth-key: MD5 hash of password
 */
class KOReaderSyncClient {
 public:
  enum Error { OK = 0, NO_CREDENTIALS, NETWORK_ERROR, AUTH_FAILED, SERVER_ERROR, JSON_ERROR, NOT_FOUND, LOW_MEMORY };

  /**
   * Get reading progress for a document.
   * @param documentHash The document hash (from KOReaderDocumentId)
   * @param outProgress Output: the progress data
   * @return OK on success, NOT_FOUND if no progress exists, error code on failure
   */
  static Error getProgress(const std::string& documentHash, KOReaderProgress& outProgress);

  /**
   * Update reading progress for a document.
   * @param progress The progress data to upload
   * @param timeoutMs TLS/HTTP deadline for this request. The 15s default is
   *   a generous upper bound; an automatic caller with nobody watching
   *   (SleepProgressSync) passes a much shorter one instead so a captive
   *   portal or black-holed server cannot stall it -- see SyncTriggerPolicy.h's
   *   AUTO_SYNC_TIMEOUT_MS.
   * @return OK on success, error code on failure
   */
  static Error updateProgress(const KOReaderProgress& progress, uint32_t timeoutMs = 15000);

  /**
   * Get human-readable error message.
   */
  static const char* errorString(Error error);

  /** HTTP status code from the last request (for diagnostics). */
  static int lastHttpCode;

#ifdef CP_TEST_CONSOLE
  // CMD:CAPTIVEPORTALBENCH -- when enabled, updateProgress() targets a
  // black-holed test address (RFC 5737 TEST-NET-1: 192.0.2.0/24, never
  // routed) instead of KOREADER_STORE's real effective base URL, so "the
  // network associates but the server never answers" can be measured on
  // hardware without touching any real credential or server state. Cleared
  // immediately after by the bench caller; never persisted -- see
  // SleepProgressSync.cpp's benchTrySyncAgainstBlackHole().
  static void setTestBlackHoleOverride(bool enabled);
#endif
};
