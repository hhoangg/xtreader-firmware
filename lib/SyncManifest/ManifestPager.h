#pragma once

#include <cstdint>
#include <string>

#include "ManifestEntry.h"

// Tracks pagination across GET /library/manifest pages: whether to fetch
// another page and, if so, the cursor to request it with. Decoupled from
// HTTP and SD entirely (mirrors lib/DevicePairing/DevicePairingPoller.h's
// split from the network call it schedules), so it is host-testable; the
// actual fetch loop that drives this and ManifestStreamParser together
// lives in SyncManifest.cpp (device-only, since it needs HttpDownloader).
//
// maxPages bounds the whole sync so a runaway or hostile server handing out
// an endless chain of nextCursor values cannot spin the device forever --
// see the task brief's "bound the total work" requirement. This is a
// different bound than LineChunker's per-line cap; that one guards a single
// line, this one guards the whole multi-page fetch.
class ManifestPager {
 public:
  // 1000 pages * the API's max limit=500 entries/page is 500,000 books --
  // already an absurd library for this device; a real hostile/broken server
  // handing out cursors forever is what this bound is actually for.
  static constexpr uint32_t DEFAULT_MAX_PAGES = 1000;

  explicit ManifestPager(uint32_t maxPages = DEFAULT_MAX_PAGES);

  // Call once per page, after that page's trailer has been parsed. Returns
  // true if another page should be fetched (nextCursor() is then the cursor
  // to request it with); false once done -- either the server said
  // nextCursor: null, or the page-count bound was hit (see
  // exceededPageLimit() to tell those two apart).
  bool onPageTrailer(const ManifestTrailer& trailer);

  const std::string& nextCursor() const { return nextCursor_; }
  uint32_t pagesFetched() const { return pagesFetched_; }
  bool exceededPageLimit() const { return exceededPageLimit_; }

 private:
  uint32_t maxPages_;
  uint32_t pagesFetched_ = 0;
  std::string nextCursor_;
  bool exceededPageLimit_ = false;
};
