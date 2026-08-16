// Fetches GET /library/manifest and writes it to /.crosspoint/remote.idx.
//
// Decisions this task leaves open (see the task brief's "report every
// decision the brief left open"):
//
//  - Only a full sync (no `since`) is implemented. The API's `since` delta
//    mode exists to avoid re-downloading a manifest that has barely changed,
//    but a delta response can carry tombstones that must be reconciled
//    against whatever the index already has -- an update-in-place or a
//    merge pass, either of which needs a second read cursor into the index
//    while the sync writes a new one. That is real complexity with no
//    consumer yet (nothing renders the index this task produces). A full
//    sync is simpler, is what "fetch the book list" for a device that has
//    never synced before actually needs, and produces the exact same file
//    format a delta-aware sync would -- so the follow-up task can add `since`
//    without touching the format or the read side at all. ManifestEntry.deleted
//    and ManifestLine's trailer parsing already understand tombstones and are
//    tested (see test/sync_manifest_parser), so that follow-up is parsing-work-free.
//  - `downloaded` is written false for every entry, always, since this task
//    does not download books. The column exists in the index format now
//    (ManifestIndexFormat.h) so a later downloader only has to flip it in
//    place, not migrate the file.
//  - Page size is the API's documented max (limit=500), to minimise the
//    number of separate HTTPS connections (each page is its own TLS
//    handshake -- HttpDownloader opens a fresh connection per fetchUrl()
//    call) a large library needs.
//  - Each index line is written with a single HalFile::write() per entry,
//    not batched through lib/Serialization/BufferedFileWriter the way
//    book-cache writes are (see that class's comment on why interleaved
//    small SD writes are otherwise slow). A sync is dominated by network
//    time, not SD time, and this way every write's success is checked
//    immediately and individually, matching ProgressFile.h's pattern
//    exactly. Worth revisiting if a real sync against a several-thousand-book
//    library turns out to be SD-bound in practice.

#include "SyncManifest.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cctype>
#include <cstdio>

#include "ManifestIndexQuery.h"
#include "ManifestPager.h"
#include "ManifestStreamParser.h"
#include "SyncCredentialStore.h"
#include "network/HttpDownloader.h"

namespace sync_manifest {

namespace {

constexpr uint32_t PAGE_LIMIT = 500;  // crosspoint-sync docs/API.md's documented max
// Small, fixed-size SD reads for the read-side queries below -- same "one
// buffer, independent of index size" reasoning as ManifestStreamParser.h,
// applied to reading instead of writing.
constexpr size_t QUERY_READ_CHUNK = 512;

HeapSample sampleHeap() {
  return HeapSample{static_cast<uint32_t>(ESP.getFreeHeap()), static_cast<uint32_t>(ESP.getMaxAllocHeap())};
}

// Matches OpdsBookBrowserActivity::handleSearchAction()'s local urlEncode
// lambda (no shared helper exists yet in this codebase to reuse instead).
std::string urlEncode(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 3);
  for (const unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

std::string manifestUrl(const std::string& cursor) {
  std::string url = SYNC_STORE.getBaseUrl() + "/library/manifest?limit=" + std::to_string(PAGE_LIMIT);
  if (!cursor.empty()) {
    url += "&cursor=" + urlEncode(cursor);
  }
  return url;
}

// Context for onManifestEntry below: the pieces of sync()'s state a
// ManifestStreamParser::EntryCallback (a plain function pointer, not a
// capturing lambda -- see CLAUDE.md's "Template and std::function Bloat")
// needs to reach. Lives on sync()'s stack for the whole call, so the
// pointers below stay valid for as long as the parser is fed.
struct EntryWriteCtx {
  HalFile* tmpFile;
  SyncResult* result;
  bool* writeFailed;
};

bool onManifestEntry(void* ctxPtr, const ManifestEntry& entry) {
  auto* ctx = static_cast<EntryWriteCtx*>(ctxPtr);
  // A full sync (no `since`) never expects a tombstone -- see the class
  // comment above for why `since` isn't wired up yet. Skip it defensively
  // rather than writing a deleted placeholder into a fresh index.
  if (entry.deleted) return true;

  ManifestIndexRecord record;
  record.id = entry.id;
  record.path = entry.path;
  record.sizeBytes = entry.sizeBytes;
  record.contentHash = entry.contentHash;
  record.updatedAt = entry.updatedAt;
  record.downloaded = false;

  const std::string line = formatIndexLine(record);
  const size_t written = ctx->tmpFile->write(line.data(), line.size());
  if (written != line.size()) {
    LOG_ERR("SYNC", "Short write to %s: %u/%u bytes", INDEX_TMP_PATH, (unsigned)written, (unsigned)line.size());
    *ctx->writeFailed = true;
    return false;
  }
  ctx->result->entriesWritten++;
  return true;
}

// Behind listByPrefix()/findById(): reads INDEX_PATH off SD in small fixed
// chunks and feeds them to `scanner` (a ManifestIndexPrefixScan or
// ManifestIndexIdLookup). Returns false only on a genuine read/parse
// error -- a missing index, or a scan that never matched anything, both
// return true, since neither is a fault.
template <typename Scanner>
bool scanIndex(Scanner& scanner) {
  if (!Storage.exists(INDEX_PATH)) return true;  // never synced yet -- not an error

  HalFile file;
  if (!Storage.openFileForRead("SYNC", INDEX_PATH, file)) {
    LOG_ERR("SYNC", "Failed to open %s for reading", INDEX_PATH);
    return false;
  }

  uint8_t buf[QUERY_READ_CHUNK];
  bool fedOk = true;
  while (fedOk) {
    const int n = file.read(buf, sizeof(buf));
    if (n <= 0) break;
    fedOk = scanner.feed(buf, static_cast<size_t>(n));
  }
  file.close();
  // fedOk is false both on a real parse error and on a deliberate early
  // stop (id found; prefix range exhausted) -- hasError() disambiguates.
  return fedOk || !scanner.hasError();
}

}  // namespace

SyncResult sync() {
  SyncResult result;
  result.beforeRequest = sampleHeap();
  result.afterHandshake = result.beforeRequest;  // overwritten once the first byte arrives, if it ever does

  if (!SYNC_STORE.isPaired()) {
    result.error = "not_paired";
    result.afterLastPage = sampleHeap();
    return result;
  }

  Storage.mkdir("/.crosspoint");  // matches PersistableStoreBase::writeDocToFile's convention

  HalFile tmpFile;
  if (!Storage.openFileForWrite("SYNC", INDEX_TMP_PATH, tmpFile)) {
    result.error = "open_tmp_failed";
    result.afterLastPage = sampleHeap();
    return result;
  }

  ManifestPager pager;
  bool handshakeSampled = false;
  bool writeFailed = false;
  std::string cursor;
  EntryWriteCtx entryCtx{&tmpFile, &result, &writeFailed};

  while (true) {
    const std::string url = manifestUrl(cursor);
    LOG_DBG("SYNC", "Fetching manifest page %u: %s (heap: %u)", (unsigned)(pager.pagesFetched() + 1), url.c_str(),
            (unsigned)ESP.getFreeHeap());

    ManifestStreamParser parser(&onManifestEntry, nullptr, &entryCtx);

    int httpStatus = -1;
    const bool fetchOk = HttpDownloader::fetchUrl(
        url,
        [&](const uint8_t* data, size_t len) -> bool {
          if (!handshakeSampled) {
            handshakeSampled = true;
            result.afterHandshake = sampleHeap();
          }
          return parser.feed(data, len);
        },
        "", "", &httpStatus, SYNC_STORE.getAccessToken());

    if (writeFailed) {
      result.error = "sd_write_failed";
      break;
    }
    if (!fetchOk || httpStatus != 200) {
      LOG_ERR("SYNC", "Manifest page fetch failed (ok=%d status=%d)", fetchOk, httpStatus);
      result.error = "fetch_failed";
      break;
    }
    if (!parser.hasTrailer()) {
      LOG_ERR("SYNC", "Manifest page ended without a trailer line");
      result.error = "missing_trailer";
      break;
    }

    result.totalCount = parser.trailer().totalCount;
    const bool more = pager.onPageTrailer(parser.trailer());
    result.pagesFetched = pager.pagesFetched();

    if (pager.exceededPageLimit()) {
      LOG_ERR("SYNC", "Manifest sync exceeded %u pages -- aborting", (unsigned)ManifestPager::DEFAULT_MAX_PAGES);
      result.error = "too_many_pages";
      break;
    }
    if (!more) {
      result.ok = true;
      break;
    }
    cursor = pager.nextCursor();
  }

  tmpFile.flush();
  tmpFile.close();

  if (!result.ok) {
    Storage.remove(INDEX_TMP_PATH);
    result.afterLastPage = sampleHeap();
    return result;
  }

  // Atomic-rename-into-place, same pattern (and same FAT caveats) as
  // src/activities/reader/ProgressFile.h: the temp file is fully written and
  // closed before this, so an interrupted sync leaves only the discardable
  // .tmp file, never a half-written remote.idx.
  Storage.remove(INDEX_PATH);
  if (!Storage.rename(INDEX_TMP_PATH, INDEX_PATH)) {
    LOG_ERR("SYNC", "Failed to rename %s into place", INDEX_TMP_PATH);
    result.ok = false;
    result.error = "rename_failed";
  }

  result.afterLastPage = sampleHeap();
  return result;
}

bool listByPrefix(const std::string& folderPrefix, ManifestIndexPrefixScan::MatchCallback onMatch, void* ctx) {
  ManifestIndexPrefixScan scan(folderPrefix, onMatch, ctx);
  return scanIndex(scan);
}

bool findById(const std::string& id, ManifestIndexRecord& out) {
  ManifestIndexIdLookup lookup(id);
  scanIndex(lookup);
  if (!lookup.found()) return false;
  out = lookup.record();
  return true;
}

bool isDownloaded(const std::string& id) {
  ManifestIndexRecord record;
  return findById(id, record) && record.downloaded;
}

}  // namespace sync_manifest
