// Reconciles /.sleep against GET /wallpapers/manifest -- see WallpaperSync.h
// for the shape of the job and lib/WallpaperSync for the decisions.
//
// Decisions this task leaves open:
//
//  - Full manifest every time, never a `since` delta, for the reason
//    WallpaperSync.h's sync() states: a detach deletes the link row, so a
//    delta has nothing to report and a delta-only device would never delete
//    anything. The cost is bounded by the cadence
//    (sync_trigger::WALLPAPER_SYNC_BOOT_INTERVAL), not by the payload.
//  - No local index file, unlike the book manifest's /.crosspoint/remote.idx.
//    The directory listing IS the index here: a wallpaper's local filename is
//    a total function of its server id (lib/WallpaperSync/WallpaperPaths.h),
//    so "what do I have" is answered by reading /.sleep and needs no second
//    copy that could drift out of step with it. That also means nothing has
//    to survive across a sleep in RAM -- every byte of state this feature
//    keeps is either the files themselves or one counter in state.json.
//  - No HTTP Range / resume, same as BookDownloader and for the same reason
//    (HttpDownloader has no Range support at all). It matters even less here:
//    a wallpaper is 96 KB, and an interrupted one is simply re-fetched whole
//    on a later sync.
//  - A wallpaper whose body does not match the manifest's sizeBytes is
//    discarded rather than renamed into place. The bytes are immutable and
//    content-identified by their id (packages/contract/src/wallpaper.ts), so
//    unlike a book -- whose size can legitimately be stale in the index --
//    a mismatch here means a truncated or wrong body, and SleepActivity
//    draws whatever is in that directory without a second opinion.
//  - Deletions run before downloads, so a card that is tight on space gets
//    the room back from wallpapers that are no longer assigned before
//    anything new is written into it.
//  - One failed download does not fail the sync. Each is independent, the
//    others are still worth having, and `moreWorkPending` brings the sync
//    back on the next boot to retry.

#include "WallpaperSync.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ManifestPager.h>
#include <Memory.h>
#include <WallpaperManifest.h>
#include <WallpaperPaths.h>
#include <WallpaperReconcile.h>

#include <algorithm>
#include <iterator>
#include <vector>

#include "SyncCredentialStore.h"
#include "network/HttpDownloader.h"

namespace wallpaper_sync {

namespace {

// The contract caps a page at 200; a reader's wallpaper set is a handful of
// rows, so one page of 50 covers it in practice and the pager exists for the
// pathological case rather than the normal one.
constexpr uint32_t PAGE_LIMIT = 50;

// SleepActivity's own filename buffer size -- the longest name SdFat will
// hand back here. Heap, not stack: CLAUDE.md's Resource Protocol caps a
// local at 256 bytes and this is exactly at that line.
constexpr size_t MAX_FILE_NAME_LEN = 256;

// A pathological /.sleep (hundreds of the reader's own pictures) must not
// turn a directory scan into an unbounded std::vector of names. Only names
// this firmware recognises as its own are collected at all, so this bound is
// far above anything MAX_LOCAL can produce and exists purely as a backstop.
constexpr size_t MAX_SCANNED_NAMES = 128;

// Refuse to start downloading if the card has less headroom than this. The
// real ceiling on what this feature ever writes is
// wallpaper_reconcile::MAX_LOCAL (~2.2 MB); this is the separate question of
// whether the card has room for it at all, and skipping cleanly beats
// filling a card to the point where progress saves start failing.
constexpr uint64_t MIN_FREE_BYTES = 4ull * 1024 * 1024;

HeapSample sampleHeap() {
  return HeapSample{static_cast<uint32_t>(ESP.getFreeHeap()), static_cast<uint32_t>(ESP.getMaxAllocHeap())};
}

std::string manifestUrl(const std::string& cursor) {
  std::string url = SYNC_STORE.getBaseUrl() + "/wallpapers/manifest?limit=" + std::to_string(PAGE_LIMIT);
  // The cursor is a wallpaper id (the manifest is ordered by id), and
  // wallpaper_paths::isValidId() already restricts those to characters that
  // need no percent-encoding -- anything else is refused rather than escaped,
  // since a cursor outside that alphabet did not come from this API.
  if (!cursor.empty() && wallpaper_paths::isValidId(cursor)) url += "&cursor=" + cursor;
  return url;
}

// One assigned wallpaper, kept only until the reconciler has decided about
// it. The size travels with the id because it is what downloadOne() checks
// the received body against.
struct Assigned {
  std::string id;
  uint64_t sizeBytes = 0;
};

// Collects assigned rows off the manifest stream. Bounded at MAX_LOCAL: this
// device will never keep more than that, and the rows past it would only be
// thrown away by the reconciler anyway.
struct CollectCtx {
  std::vector<Assigned>* assigned;
  uint32_t aliveSeen = 0;
};

bool onManifestEntry(void* ctxPtr, const wallpaper_manifest::Entry& entry) {
  auto* ctx = static_cast<CollectCtx*>(ctxPtr);
  if (entry.deleted) return true;  // a tombstone is not an assignment
  ctx->aliveSeen++;
  if (ctx->assigned->size() < wallpaper_reconcile::MAX_LOCAL) {
    ctx->assigned->push_back(Assigned{entry.id, entry.sizeBytes});
  }
  return true;
}

uint64_t sizeOf(const std::vector<Assigned>& assigned, const std::string& id) {
  const auto it = std::find_if(assigned.begin(), assigned.end(), [&id](const Assigned& row) { return row.id == id; });
  return it == assigned.end() ? 0 : it->sizeBytes;
}

// Every filename currently in `dir`, filtered to the ones
// wallpaper_paths::classifyFileName() recognises as this firmware's own.
// The reader's files are skipped here as an optimisation only -- the
// reconciler applies the very same rule again, and it is the tested one.
void scanWallpaperDir(const char* dir, std::vector<std::string>& outNames) {
  if (!Storage.exists(dir)) return;

  HalFile handle = Storage.open(dir);
  if (!handle || !handle.isDirectory()) {
    LOG_ERR("WALLP", "%s is not a directory", dir);
    return;
  }

  auto name = makeUniqueNoThrow<char[]>(MAX_FILE_NAME_LEN);
  if (!name) {
    LOG_ERR("WALLP", "OOM: %u byte filename buffer", static_cast<unsigned>(MAX_FILE_NAME_LEN));
    return;
  }

  for (auto entry = handle.openNextFile(); entry; entry = handle.openNextFile()) {
    if (entry.isDirectory()) continue;
    name[0] = '\0';
    entry.getName(name.get(), MAX_FILE_NAME_LEN);
    if (name[0] == '\0') continue;
    if (wallpaper_paths::classifyFileName(name.get()) == wallpaper_paths::FileKind::Unmanaged) continue;
    if (outNames.size() >= MAX_SCANNED_NAMES) {
      // Truncating only ever means missing a deletion or repeating a
      // download, never deleting the wrong thing.
      LOG_ERR("WALLP", "More than %u managed files in %s -- scan truncated", static_cast<unsigned>(MAX_SCANNED_NAMES),
              dir);
      break;
    }
    outNames.emplace_back(name.get());
  }
}

// Context for downloadOne()'s streaming callback. Plain pointers, not a
// capturing closure, for the same reason StreamCtx in BookDownloader.cpp is.
struct StreamCtx {
  HalFile* tmpFile;
  uint64_t downloaded = 0;
  bool writeFailed = false;
};

// Streams one wallpaper into `dir` via a ".part" file renamed into place on
// success, exactly as book_downloader::download() does: an interrupted
// download can leave only a discardable ".part", never a truncated .bmp
// where SleepActivity would try to draw it.
bool downloadOne(const std::string& id, const uint64_t expectedSize, const char* dir, const uint32_t timeoutMs) {
  const std::string fileName = wallpaper_paths::fileNameForId(id);
  if (fileName.empty()) {
    LOG_ERR("WALLP", "Refusing id with an unusable shape");
    return false;
  }
  const std::string destPath = wallpaper_paths::joinPath(dir, fileName);
  const std::string tmpPath = wallpaper_paths::joinPath(dir, wallpaper_paths::tempNameFor(fileName));

  Storage.remove(tmpPath.c_str());  // discard any stale attempt from a previous crash

  HalFile tmpFile;
  if (!Storage.openFileForWrite("WALLP", tmpPath, tmpFile)) {
    LOG_ERR("WALLP", "Failed to open %s for writing", tmpPath.c_str());
    return false;
  }

  StreamCtx ctx;
  ctx.tmpFile = &tmpFile;

  const std::string url = SYNC_STORE.getBaseUrl() + "/wallpapers/" + id + "/file";
  int httpStatus = -1;
  const bool fetchOk = HttpDownloader::fetchUrl(
      url,
      [&ctx](const uint8_t* data, size_t len) -> bool {
        const size_t written = ctx.tmpFile->write(data, len);
        if (written != len) {
          ctx.writeFailed = true;
          return false;
        }
        ctx.downloaded += len;
        return true;
      },
      "", "", &httpStatus, SYNC_STORE.getAccessToken(), timeoutMs);

  // Close before the remove()/rename() below touch the same paths (see
  // CLAUDE.md's DESTRUCTOR_CLOSES_FILE "close before delete/reopen" cases).
  tmpFile.flush();
  tmpFile.close();

  const bool sizeOk = expectedSize == 0 || ctx.downloaded == expectedSize;
  if (!fetchOk || httpStatus != 200 || ctx.writeFailed || ctx.downloaded == 0 || !sizeOk) {
    LOG_ERR("WALLP", "Download of %s failed (ok=%d status=%d write=%d got=%llu want=%llu)", id.c_str(), fetchOk,
            httpStatus, static_cast<int>(ctx.writeFailed), static_cast<unsigned long long>(ctx.downloaded),
            static_cast<unsigned long long>(expectedSize));
    Storage.remove(tmpPath.c_str());
    return false;
  }

  Storage.remove(destPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), destPath.c_str())) {
    LOG_ERR("WALLP", "Failed to rename %s into place", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }

  LOG_DBG("WALLP", "Downloaded %llu bytes to %s (heap: %u)", static_cast<unsigned long long>(ctx.downloaded),
          destPath.c_str(), static_cast<unsigned>(ESP.getFreeHeap()));
  return true;
}

bool haveRoomOnCard() {
  const uint64_t total = Storage.sdTotalBytes();
  if (total == 0) return true;  // capacity unknown -- do not block on a number we do not have
  const uint64_t used = Storage.sdUsedBytes();
  if (used == 0 || used > total) return true;
  return (total - used) >= MIN_FREE_BYTES;
}

}  // namespace

SyncResult sync(const uint32_t timeoutMs, const ProgressCallback onProgress, void* progressCtx) {
  SyncResult result;
  result.beforeRequest = sampleHeap();
  result.afterHandshake = result.beforeRequest;  // overwritten once a first byte arrives, if one ever does
  result.afterSync = result.beforeRequest;

  if (!SYNC_STORE.isPaired()) {
    result.error = "not_paired";
    return result;
  }

  std::vector<Assigned> assigned;
  assigned.reserve(wallpaper_reconcile::MAX_LOCAL);
  CollectCtx collect{&assigned, 0};

  ManifestPager pager;
  bool handshakeSampled = false;
  std::string cursor;

  while (true) {
    const std::string url = manifestUrl(cursor);
    LOG_DBG("WALLP", "Fetching wallpaper manifest page %u (heap: %u)", static_cast<unsigned>(pager.pagesFetched() + 1),
            static_cast<unsigned>(ESP.getFreeHeap()));

    wallpaper_manifest::StreamParser parser(&onManifestEntry, nullptr, &collect);

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

    result.httpStatus = httpStatus;
    if (!fetchOk || httpStatus != 200) {
      LOG_ERR("WALLP", "Wallpaper manifest page fetch failed (ok=%d status=%d)", fetchOk, httpStatus);
      result.error = "fetch_failed";
      result.afterSync = sampleHeap();
      return result;
    }
    if (!parser.hasTrailer()) {
      LOG_ERR("WALLP", "Wallpaper manifest page ended without a trailer line");
      result.error = "missing_trailer";
      result.afterSync = sampleHeap();
      return result;
    }

    const bool more = pager.onPageTrailer(parser.trailer());
    result.pagesFetched = pager.pagesFetched();
    if (pager.exceededPageLimit()) {
      LOG_ERR("WALLP", "Wallpaper manifest exceeded %u pages -- aborting",
              static_cast<unsigned>(ManifestPager::DEFAULT_MAX_PAGES));
      result.error = "too_many_pages";
      result.afterSync = sampleHeap();
      return result;
    }
    // Everything past MAX_LOCAL is discarded by the reconciler anyway, so
    // stop paying for pages that can only add ids destined for the bin.
    if (!more || assigned.size() >= wallpaper_reconcile::MAX_LOCAL) break;
    cursor = pager.nextCursor();
  }

  result.assignedCount = collect.aliveSeen;

  std::vector<std::string> assignedIds;
  assignedIds.reserve(assigned.size());
  std::transform(assigned.begin(), assigned.end(), std::back_inserter(assignedIds),
                 [](const Assigned& row) { return row.id; });

  std::vector<std::string> localNames;
  scanWallpaperDir(SLEEP_DIR, localNames);

  const wallpaper_reconcile::ReconcilePlan reconciled = wallpaper_reconcile::plan(assignedIds, localNames);
  if (reconciled.droppedForLocalCap > 0) {
    LOG_INF("WALLP", "%u assigned wallpaper(s) past this device's cap of %u -- not fetched",
            static_cast<unsigned>(reconciled.droppedForLocalCap),
            static_cast<unsigned>(wallpaper_reconcile::MAX_LOCAL));
  }

  for (const std::string& name : reconciled.deleteNames) {
    const std::string path = wallpaper_paths::joinPath(SLEEP_DIR, name);
    if (Storage.remove(path.c_str())) {
      result.deleted++;
      LOG_DBG("WALLP", "Removed %s", path.c_str());
    } else {
      LOG_ERR("WALLP", "Failed to remove %s", path.c_str());
    }
  }

  result.ok = true;
  result.moreWorkPending = reconciled.moreWorkPending;

  if (reconciled.downloadIds.empty()) {
    result.afterSync = sampleHeap();
    return result;
  }
  if (!haveRoomOnCard()) {
    LOG_ERR("WALLP", "Skipping %u wallpaper download(s): less than %u MB free on the card",
            static_cast<unsigned>(reconciled.downloadIds.size()),
            static_cast<unsigned>(MIN_FREE_BYTES / (1024 * 1024)));
    result.error = "low_space";
    result.moreWorkPending = true;
    result.afterSync = sampleHeap();
    return result;
  }

  if (!Storage.exists(SLEEP_DIR) && !Storage.mkdir(SLEEP_DIR)) {
    LOG_ERR("WALLP", "Failed to create %s", SLEEP_DIR);
    result.ok = false;
    result.error = "mkdir_failed";
    result.afterSync = sampleHeap();
    return result;
  }

  const uint32_t total = static_cast<uint32_t>(reconciled.downloadIds.size());
  uint32_t done = 0;
  for (const std::string& id : reconciled.downloadIds) {
    if (downloadOne(id, sizeOf(assigned, id), SLEEP_DIR, sync_trigger::EXPLICIT_SYNC_TIMEOUT_MS)) {
      result.downloaded++;
    } else {
      result.failed++;
      result.moreWorkPending = true;
    }
    done++;
    if (onProgress) onProgress(progressCtx, done, total);
  }

  result.afterSync = sampleHeap();
  LOG_INF("WALLP", "Wallpaper sync: %u assigned, %u downloaded, %u deleted, %u failed (heap: %u)",
          static_cast<unsigned>(result.assignedCount), static_cast<unsigned>(result.downloaded),
          static_cast<unsigned>(result.deleted), static_cast<unsigned>(result.failed),
          static_cast<unsigned>(ESP.getFreeHeap()));
  return result;
}

}  // namespace wallpaper_sync
