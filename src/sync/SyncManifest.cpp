// Fetches GET /library/manifest and persists it to /.crosspoint/remote.idx as a full listing or a
// `since` delta, merged onto whatever is already there -- see SyncManifest.h's sync() doc comment for
// which one happens when, and ManifestIndexMerge.h for the primitive the delta merge and
// removeFromIndex() below both build on.
//
// Decisions this task leaves open (see the task brief's "report every decision the brief left open"):
//
//  - The delta watermark is the maximum `updatedAt` seen across every entry (including tombstones --
//    a deletion is a change too) this sync received, written into the index's own header once the
//    whole sync succeeds. The server's delta scope is `updatedAt >= since` (inclusive -- see
//    crosspoint-sync's routes/library.ts), so passing that same maximum back next time re-fetches
//    anything sharing its exact second, which is a harmless idempotent re-upsert, not a gap. An empty
//    delta (nothing changed) leaves the watermark exactly where it was -- there is nothing new to
//    advance past. Not covered: a book edited concurrently, mid-pagination, at a path this sync's
//    keyset cursor has already moved past (pagination is by path, not time, so a page already fetched
//    is never re-queried). If that edit's `updatedAt` happens to be less than the max this sync did
//    see, it will not satisfy `since` on any later sync either, and is missed until a full resync.
//    Closing that gap needs either snapshot isolation or a monotonic change-sequence cursor from the
//    server, neither of which this API exposes; for a single-owner personal library synced from one
//    client at a time (never two devices racing a write against the same account mid-sync), this is
//    an accepted, documented residual risk rather than one worth a heavier protocol change.
//  - A delta's upserts (new books and updates alike) are always written with `downloaded=false`,
//    matching a full sync's existing behaviour -- this index has no way to know whether an updated
//    row's local file (if any) still matches, and guessing wrong in the optimistic direction would
//    hide a stale/renamed local copy behind a "downloaded" row that never gets refreshed. The
//    consequence: a server-side rename of an already-downloaded book shows as an "On server"
//    placeholder again until it's (re)downloaded or the old local file is found and matched by hand --
//    no worse than a full sync's already-existing behaviour for the same case (see docs/API.md's own
//    "rename the local file instead of re-downloading" note, which nothing in this codebase implements
//    yet; that is separate, larger scope).
//  - Page size is the API's documented max (limit=500), to minimise the number of separate HTTPS
//    connections (each page is its own TLS handshake -- HttpDownloader opens a fresh connection per
//    fetchUrl() call) a large library needs.
//  - Each index line is written with a single HalFile::write() per entry, not batched through
//    lib/Serialization/BufferedFileWriter the way book-cache writes are (see that class's comment on
//    why interleaved small SD writes are otherwise slow). A sync is dominated by network time, not SD
//    time, and this way every write's success is checked immediately and individually, matching
//    ProgressFile.h's pattern exactly. Worth revisiting if a real sync against a several-thousand-book
//    library turns out to be SD-bound in practice.
//  - A delta merge that finds the *existing* index unreadable mid-merge (a corrupt or truncated
//    remote.idx whose header nonetheless parsed -- e.g. torn mid-write despite the temp-then-rename
//    protection, or a flipped bit) retries once as a full sync within the same sync() call, rather
//    than surfacing an error a caller would have to translate into "delete .crosspoint and try again"
//    for the user. A header that fails to parse at all is cheaper to detect and already routes to a
//    full sync up front, before any network request -- see openIndexForScan().

#include "SyncManifest.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <utility>

#include "ManifestIndexMerge.h"
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

std::string manifestUrl(const std::string& cursor, const bool deltaMode, const uint64_t since) {
  std::string url = SYNC_STORE.getBaseUrl() + "/library/manifest?limit=" + std::to_string(PAGE_LIMIT);
  if (deltaMode) {
    url += "&since=" + std::to_string(since);
  }
  if (!cursor.empty()) {
    url += "&cursor=" + urlEncode(cursor);
  }
  return url;
}

// Opens `path` for `mode` and validates its header (see ManifestIndexFormat.h): a missing file, an
// unopenable one, a short/malformed header, or a version other than INDEX_FORMAT_VERSION are all
// reported the same way -- false, `outFile` left closed -- since every caller here treats "no usable
// header" as "no usable index" regardless of which of those it was (see ManifestIndexFormat.h's
// header comment for why they're deliberately not distinguished further). `version`/`watermark` are
// only meaningful when this returns true.
bool openIndexForScan(const char* path, const oflag_t mode, HalFile& outFile, uint32_t& version, uint64_t& watermark) {
  version = 0;
  watermark = 0;
  if (!Storage.exists(path)) return false;

  HalFile file = Storage.open(path, mode);
  if (!file) {
    LOG_ERR("SYNC", "Failed to open %s (mode=%d)", path, static_cast<int>(mode));
    return false;
  }

  uint8_t headerBuf[INDEX_HEADER_LEN];
  const int n = file.read(headerBuf, sizeof(headerBuf));
  if (n != static_cast<int>(sizeof(headerBuf)) ||
      !parseIndexHeader(reinterpret_cast<const char*>(headerBuf), sizeof(headerBuf), version, watermark) ||
      version != INDEX_FORMAT_VERSION) {
    LOG_DBG("SYNC", "%s has no usable header (read %d bytes) -- treating as unsynced", path, n);
    return false;
  }

  outFile = std::move(file);
  return true;
}

// Behind listByPrefix()/findById(): reads INDEX_PATH off SD in small fixed
// chunks (past its header) and feeds them to `scanner` (a
// ManifestIndexPrefixScan or ManifestIndexIdLookup). Returns false only on a
// genuine read/parse error -- a missing or headerless index, or a scan that
// never matched anything, both return true, since neither is a fault.
template <typename Scanner>
bool scanIndex(Scanner& scanner) {
  HalFile file;
  uint32_t version = 0;
  uint64_t watermark = 0;
  if (!openIndexForScan(INDEX_PATH, O_RDONLY, file, version, watermark))
    return true;  // no usable index -- not an error
  (void)version;
  (void)watermark;

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

// Context threaded through onManifestEntry/onManifestTrailer for the duration of one performSync()
// call. Lives on performSync()'s stack, so the pointers below stay valid for as long as the parser
// is fed -- same convention as the rest of this file's plain-function-pointer callbacks (see
// CLAUDE.md's "Template and std::function Bloat").
struct EntryCollectCtx {
  bool deltaMode;
  HalFile* tmpFile;                           // full mode only: entries are written straight through
  std::vector<std::string>* removeIds;        // delta mode only
  std::vector<ManifestIndexRecord>* upserts;  // delta mode only
  bool reservedOnce;                          // delta mode only: whether the first trailer's totalCount has been used
  SyncResult* result;
  bool* writeFailed;
  uint64_t* maxUpdatedAt;
};

bool onManifestEntry(void* ctxPtr, const ManifestEntry& entry) {
  auto* ctx = static_cast<EntryCollectCtx*>(ctxPtr);
  if (entry.updatedAt > *ctx->maxUpdatedAt) *ctx->maxUpdatedAt = entry.updatedAt;

  if (ctx->deltaMode) {
    // Evict any stale copy of this id from the old index regardless of whether this is an update or
    // a tombstone -- a no-op if the id wasn't there (a brand new book). See ManifestIndexMerge's
    // class comment for why an update is a removal-plus-insert rather than an in-place edit.
    ctx->removeIds->push_back(entry.id);
    if (!entry.deleted) {
      ManifestIndexRecord record;
      record.id = entry.id;
      record.path = entry.path;
      record.sizeBytes = entry.sizeBytes;
      record.contentHash = entry.contentHash;
      record.updatedAt = entry.updatedAt;
      record.downloaded = false;  // see this file's top comment
      ctx->upserts->push_back(std::move(record));
    }
    ctx->result->entriesWritten++;
    return true;
  }

  // Full mode: a `since`-less scope never sends a tombstone (crosspoint-sync's routes/library.ts
  // scopes it to alive rows only) -- skip defensively rather than writing a deleted placeholder into
  // a fresh index.
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

// Reserves removeIds/upserts to the delta's total size as soon as it's known (the first page's
// trailer -- crosspoint-sync's routes/library.ts counts the whole filtered scope up front, not just
// one page), matching CLAUDE.md's "reserve before push_back" rule. Both vectors share one reserve
// call sized to the delta's total row count -- an upper bound for either since every row is either a
// removal, an upsert, or both, never neither.
void onManifestTrailerForReserve(void* ctxPtr, const ManifestTrailer& trailer) {
  auto* ctx = static_cast<EntryCollectCtx*>(ctxPtr);
  if (!ctx->deltaMode || ctx->reservedOnce) return;
  ctx->removeIds->reserve(trailer.totalCount);
  ctx->upserts->reserve(trailer.totalCount);
  ctx->reservedOnce = true;
}

// Sink for ManifestIndexMerge's onRecord during a delta merge: formats and writes each surviving
// record straight to the new tmp index, the same one-write-per-entry pattern onManifestEntry's full-
// mode branch uses.
struct MergeWriteCtx {
  HalFile* tmpFile;
  uint32_t written = 0;
  bool failed = false;
};

bool writeMergedRecord(void* ctxPtr, const ManifestIndexRecord& record) {
  auto* ctx = static_cast<MergeWriteCtx*>(ctxPtr);
  const std::string line = formatIndexLine(record);
  const size_t written = ctx->tmpFile->write(line.data(), line.size());
  if (written != line.size()) {
    LOG_ERR("SYNC", "Short write to %s: %u/%u bytes", INDEX_TMP_PATH, (unsigned)written, (unsigned)line.size());
    ctx->failed = true;
    return false;
  }
  ctx->written++;
  return true;
}

// One full attempt: fetch the manifest (full or delta, per `deltaMode`/`sinceWatermark`), write the
// result to INDEX_TMP_PATH, and rename it into place. On a delta whose merge finds the *existing*
// index corrupt mid-scan, returns error="corrupt_index" without renaming anything -- sync() below is
// what turns that into a full-sync retry.
SyncResult performSync(const bool deltaMode, const uint64_t sinceWatermark, const uint32_t timeoutMs) {
  SyncResult result;
  result.deltaSync = deltaMode;
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

  // Reserve the header's fixed-width slot now; patched with the real version/watermark once the
  // whole sync (and, in delta mode, the merge against the old index) succeeds -- see
  // ManifestIndexFormat.h's header comment for why a fixed width makes that safe.
  const std::string placeholderHeader = formatIndexHeader(0, 0);
  if (placeholderHeader.size() != INDEX_HEADER_LEN ||
      tmpFile.write(placeholderHeader.data(), placeholderHeader.size()) != placeholderHeader.size()) {
    LOG_ERR("SYNC", "Failed to write placeholder header to %s", INDEX_TMP_PATH);
    tmpFile.close();
    Storage.remove(INDEX_TMP_PATH);
    result.error = "sd_write_failed";
    result.afterLastPage = sampleHeap();
    return result;
  }

  ManifestPager pager;
  bool handshakeSampled = false;
  bool writeFailed = false;
  std::string cursor;
  uint64_t maxUpdatedAt = 0;

  std::vector<std::string> removeIds;
  std::vector<ManifestIndexRecord> upserts;
  EntryCollectCtx entryCtx{deltaMode, &tmpFile, &removeIds, &upserts, false, &result, &writeFailed, &maxUpdatedAt};

  while (true) {
    const std::string url = manifestUrl(cursor, deltaMode, sinceWatermark);
    LOG_DBG("SYNC", "Fetching manifest page %u: %s (heap: %u)", (unsigned)(pager.pagesFetched() + 1), url.c_str(),
            (unsigned)ESP.getFreeHeap());

    ManifestStreamParser parser(&onManifestEntry, &onManifestTrailerForReserve, &entryCtx);

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
        "", "", &httpStatus, SYNC_STORE.getAccessToken(), timeoutMs);

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

  if (!result.ok) {
    tmpFile.close();
    Storage.remove(INDEX_TMP_PATH);
    result.afterLastPage = sampleHeap();
    return result;
  }

  // An empty delta (nothing changed) must not regress the watermark -- there is nothing new to
  // advance past, and doing so would just make the next sync redo the same work for no reason.
  uint64_t newWatermark = deltaMode ? sinceWatermark : 0;
  if (maxUpdatedAt > newWatermark) newWatermark = maxUpdatedAt;

  if (deltaMode) {
    std::sort(upserts.begin(), upserts.end(),
              [](const ManifestIndexRecord& a, const ManifestIndexRecord& b) { return a.path < b.path; });

    HalFile oldFile;
    uint32_t oldVersion = 0;
    uint64_t oldWatermarkUnused = 0;
    const bool haveOld = openIndexForScan(INDEX_PATH, O_RDONLY, oldFile, oldVersion, oldWatermarkUnused);
    // haveOld should always be true here -- sync() only calls performSync(deltaMode=true) once it has
    // already confirmed a valid header -- but a false here still degrades gracefully: the merge below
    // just becomes "every upsert, nothing to remove from", i.e. a fresh index built from the delta
    // alone, rather than aborting an otherwise-successful network fetch.

    MergeWriteCtx writeCtx{&tmpFile};
    ManifestIndexMerge merge(std::move(removeIds), std::move(upserts), &writeMergedRecord, &writeCtx);

    bool mergeOk = true;
    if (haveOld) {
      uint8_t buf[QUERY_READ_CHUNK];
      while (mergeOk) {
        const int n = oldFile.read(buf, sizeof(buf));
        if (n <= 0) break;
        mergeOk = merge.feed(buf, static_cast<size_t>(n));
      }
      oldFile.close();  // done reading; close before this function's later rename touches INDEX_PATH
    }
    if (mergeOk) mergeOk = merge.finish();

    if (!mergeOk || merge.hasError() || writeCtx.failed) {
      LOG_ERR("SYNC", "Delta merge failed against existing %s -- caller should retry as a full sync", INDEX_PATH);
      tmpFile.close();
      Storage.remove(INDEX_TMP_PATH);
      result.ok = false;
      result.error = "corrupt_index";
      result.afterLastPage = sampleHeap();
      return result;
    }
  }

  // Patch the header now that the true final watermark (and, in delta mode, the merge) are known --
  // fixed width, so this seek-and-overwrite cannot disturb anything written after it.
  tmpFile.seekSet(0);
  const std::string finalHeader = formatIndexHeader(INDEX_FORMAT_VERSION, newWatermark);
  const bool headerOk = finalHeader.size() == INDEX_HEADER_LEN &&
                        tmpFile.write(finalHeader.data(), finalHeader.size()) == finalHeader.size();
  tmpFile.flush();
  tmpFile.close();
  if (!headerOk) {
    LOG_ERR("SYNC", "Failed to patch final header in %s", INDEX_TMP_PATH);
    Storage.remove(INDEX_TMP_PATH);
    result.ok = false;
    result.error = "sd_write_failed";
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

}  // namespace

SyncResult sync(const uint32_t timeoutMs) {
  HalFile existingIndex;
  uint32_t existingVersion = 0;
  uint64_t existingWatermark = 0;
  // Just peeking at the header to decide full vs. delta -- performSync() reopens the file itself for
  // the actual merge, so this handle is released immediately either way.
  const bool haveWatermark = openIndexForScan(INDEX_PATH, O_RDONLY, existingIndex, existingVersion, existingWatermark);
  if (existingIndex) existingIndex.close();

  SyncResult result = performSync(haveWatermark, existingWatermark, timeoutMs);
  if (!result.ok && result.error == "corrupt_index") {
    LOG_ERR("SYNC", "Falling back to a full resync after a corrupt existing index");
    result = performSync(false, 0, timeoutMs);
  }
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

namespace {
// findIdByPath()'s listByPrefix callback context/trampoline -- plain
// function pointer, not a capturing lambda (see CLAUDE.md's "Template and
// std::function Bloat"), matching feedRemoteRecordToMerge's style in
// FileBrowserActivity.cpp.
struct PathMatch {
  std::string wantPath;
  std::string id;
  bool found = false;
};

bool onPathCandidate(void* ctxPtr, const ManifestIndexRecord& record) {
  auto* m = static_cast<PathMatch*>(ctxPtr);
  if (record.path != m->wantPath) return true;  // same-prefix, not an exact match -- keep scanning
  m->id = record.id;
  m->found = true;
  return false;  // exact match found -- stop
}
}  // namespace

bool findIdByPath(const std::string& path, std::string& outId) {
  PathMatch match;
  match.wantPath = path;
  if (!listByPrefix(path, &onPathCandidate, &match)) return false;
  if (!match.found) return false;
  outId = match.id;
  return true;
}

bool isDownloaded(const std::string& id) {
  ManifestIndexRecord record;
  return findById(id, record) && record.downloaded;
}

bool markDownloaded(const std::string& id) {
  HalFile file;
  uint32_t version = 0;
  uint64_t watermark = 0;
  if (!openIndexForScan(INDEX_PATH, O_RDWR, file, version, watermark)) {
    LOG_ERR("SYNC", "markDownloaded: no usable index to update for id %s", id.c_str());
    return false;
  }
  (void)version;
  (void)watermark;

  ManifestIndexDownloadedFlagLocator locator(id);
  uint8_t buf[QUERY_READ_CHUNK];
  while (!locator.found() && !locator.hasError()) {
    const int n = file.read(buf, sizeof(buf));
    if (n <= 0) break;
    locator.feed(buf, static_cast<size_t>(n));
  }

  if (locator.hasError()) {
    LOG_ERR("SYNC", "Corrupt index line while looking for id %s in %s", id.c_str(), INDEX_PATH);
    return false;
  }
  if (!locator.found()) {
    LOG_ERR("SYNC", "markDownloaded: id %s not found in %s", id.c_str(), INDEX_PATH);
    return false;
  }

  // locator.flagOffset() is relative to the bytes fed to it (everything past the header) -- the
  // absolute file offset is that plus the header's own fixed width.
  if (!file.seekSet(INDEX_HEADER_LEN + locator.flagOffset())) {
    LOG_ERR("SYNC", "Failed to seek to flag offset for id %s in %s", id.c_str(), INDEX_PATH);
    return false;
  }
  const uint8_t one = '1';
  const size_t written = file.write(&one, 1);
  file.flush();
  if (written != 1) {
    LOG_ERR("SYNC", "Short write flipping downloaded flag for id %s in %s", id.c_str(), INDEX_PATH);
    return false;
  }
  return true;
}

bool removeFromIndex(const std::string& id) {
  HalFile oldFile;
  uint32_t version = 0;
  uint64_t watermark = 0;
  if (!openIndexForScan(INDEX_PATH, O_RDONLY, oldFile, version, watermark)) {
    // No usable index to remove from -- not an error (see this function's header comment): the row
    // this was meant to clear either doesn't exist yet or will be gone the next time a sync replaces
    // the whole file anyway.
    return true;
  }

  Storage.mkdir("/.crosspoint");
  HalFile tmpFile;
  if (!Storage.openFileForWrite("SYNC", INDEX_TMP_PATH, tmpFile)) {
    LOG_ERR("SYNC", "removeFromIndex: failed to open %s", INDEX_TMP_PATH);
    oldFile.close();
    return false;
  }

  // The header carries no information this removal changes (format version and watermark are both
  // untouched by dropping one record), so it's written once, up front, with its final value --
  // unlike sync()'s placeholder-then-patch, there's nothing here still to be decided.
  const std::string header = formatIndexHeader(version, watermark);
  if (header.size() != INDEX_HEADER_LEN || tmpFile.write(header.data(), header.size()) != header.size()) {
    LOG_ERR("SYNC", "removeFromIndex: failed to write header to %s", INDEX_TMP_PATH);
    tmpFile.close();
    Storage.remove(INDEX_TMP_PATH);
    oldFile.close();
    return false;
  }

  MergeWriteCtx writeCtx{&tmpFile};
  std::vector<std::string> removeIds{id};
  ManifestIndexMerge merge(std::move(removeIds), {}, &writeMergedRecord, &writeCtx);

  bool mergeOk = true;
  uint8_t buf[QUERY_READ_CHUNK];
  while (mergeOk) {
    const int n = oldFile.read(buf, sizeof(buf));
    if (n <= 0) break;
    mergeOk = merge.feed(buf, static_cast<size_t>(n));
  }
  oldFile.close();
  if (mergeOk) mergeOk = merge.finish();

  tmpFile.flush();
  tmpFile.close();

  if (!mergeOk || merge.hasError() || writeCtx.failed) {
    LOG_ERR("SYNC", "removeFromIndex: merge failed for id %s", id.c_str());
    Storage.remove(INDEX_TMP_PATH);
    return false;
  }

  Storage.remove(INDEX_PATH);
  if (!Storage.rename(INDEX_TMP_PATH, INDEX_PATH)) {
    LOG_ERR("SYNC", "removeFromIndex: failed to rename %s into place", INDEX_TMP_PATH);
    return false;
  }
  return true;
}

}  // namespace sync_manifest
