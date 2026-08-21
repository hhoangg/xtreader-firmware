// Decisions this task leaves open (see the task brief's "report every
// decision the brief left open"):
//
//  - No HTTP Range / resume. GET /library/:id/file forwards Range and passes
//    206 through (crosspoint-sync docs/API.md), so the server side already
//    supports it -- but HttpDownloader has no Range support at all today (no
//    "Range" header, no partial-content handling, no byte-offset bookkeeping
//    anywhere in it), so adding resume here means adding it there first.
//    Books in this library are a few MB; the reader is used indoors on
//    ordinary home Wi-Fi; and a failed download simply restarts from zero
//    next time the queue reaches it -- annoying, not broken, and no worse
//    than what happens today with a book copied over manually and
//    interrupted. Resume earns its complexity only once large books over a
//    flaky connection turn out to be a real support burden in practice. The
//    seam is left open on purpose: tempPathFor()'s temp file already holds
//    exactly the bytes a Range-based resume would want to keep rather than
//    discard, and DownloadResult.bytesDownloaded already reports how far a
//    failed attempt got -- a future resume only has to teach
//    HttpDownloader a starting offset and stop deleting the temp file on
//    failure, not restructure this call.
//  - Progress is reported against the manifest's `sizeBytes`, not a
//    Content-Length this module reads back off the response. The server can
//    change a book's bytes on disk without this device's index having
//    re-synced yet, so the progress bar can be very slightly off in that
//    edge case -- acceptable for a progress bar, and it avoids adding a
//    content-length-reading code path to HttpDownloader (which the
//    DataCallback overload used here does not expose) for a number this
//    module already has for free.
//  - A failed markDownloaded() (the index write) does not fail the download
//    itself: DownloadResult.ok is about whether the book's bytes are safely
//    on SD, which is already true by the time markDownloaded() runs. Losing
//    the index's bookkeeping just means a future manifest sync's
//    isDownloaded() check is stale for this one book -- annoying (a
//    redundant re-download offer), not data loss.

#include "BookDownloader.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include "DownloadPaths.h"
#include "SyncCredentialStore.h"
#include "SyncManifest.h"
#include "network/HttpDownloader.h"

namespace book_downloader {

namespace {

HeapSample sampleHeap() {
  return HeapSample{static_cast<uint32_t>(ESP.getFreeHeap()), static_cast<uint32_t>(ESP.getMaxAllocHeap())};
}

// Context for the fetchUrl DataCallback below -- plain pointers, not a
// capturing lambda closure, so this stays a function-pointer-shaped callback
// all the way down (see CLAUDE.md's "Template and std::function Bloat");
// fetchUrl's DataCallback parameter itself is a std::function only because
// HttpDownloader predates that rule -- see this file's callers for the
// no-std::function boundary this module itself exposes.
struct StreamCtx {
  HalFile* tmpFile;
  uint64_t downloaded = 0;
  uint64_t expectedTotal = 0;
  bool handshakeSampled = false;
  bool writeFailed = false;
  bool cancelled = false;
  bool* cancelFlag;
  ProgressCallback onProgress;
  void* progressCtx;
  HeapSample afterHandshake;
};

}  // namespace

DownloadResult download(const std::string& id, ProgressCallback onProgress, void* progressCtx, bool* cancelFlag) {
  DownloadResult result;
  result.beforeRequest = sampleHeap();
  result.afterHandshake = result.beforeRequest;  // overwritten once the first byte arrives, if it ever does
  result.afterDownload = result.beforeRequest;

  if (!SYNC_STORE.isPaired()) {
    result.error = "not_paired";
    return result;
  }

  ManifestIndexRecord record;
  if (!sync_manifest::findById(id, record)) {
    result.error = "not_found";
    return result;
  }
  result.destPath = record.path;

  const std::string parentDir = book_download_paths::parentDirOf(record.path);
  if (!parentDir.empty() && parentDir != "/" && !Storage.exists(parentDir.c_str())) {
    if (!Storage.mkdir(parentDir.c_str())) {
      LOG_ERR("BOOKDL", "Failed to create %s", parentDir.c_str());
      result.error = "mkdir_failed";
      result.afterDownload = sampleHeap();
      return result;
    }
  }

  const std::string tmpPath = book_download_paths::tempPathFor(record.path);
  Storage.remove(tmpPath.c_str());  // discard any stale attempt from a previous crash

  HalFile tmpFile;
  if (!Storage.openFileForWrite("BOOKDL", tmpPath, tmpFile)) {
    LOG_ERR("BOOKDL", "Failed to open %s for writing", tmpPath.c_str());
    result.error = "open_tmp_failed";
    result.afterDownload = sampleHeap();
    return result;
  }

  const std::string url = SYNC_STORE.getBaseUrl() + "/library/" + id + "/file";
  LOG_DBG("BOOKDL", "Downloading id=%s -> %s (heap: %u)", id.c_str(), record.path.c_str(), (unsigned)ESP.getFreeHeap());

  StreamCtx ctx;
  ctx.tmpFile = &tmpFile;
  ctx.expectedTotal = record.sizeBytes;
  ctx.cancelFlag = cancelFlag;
  ctx.onProgress = onProgress;
  ctx.progressCtx = progressCtx;

  int httpStatus = -1;
  const bool fetchOk = HttpDownloader::fetchUrl(
      url,
      [&ctx](const uint8_t* data, size_t len) -> bool {
        if (!ctx.handshakeSampled) {
          ctx.handshakeSampled = true;
          ctx.afterHandshake = sampleHeap();
        }
        if (ctx.cancelFlag && *ctx.cancelFlag) {
          ctx.cancelled = true;
          return false;
        }
        const size_t written = ctx.tmpFile->write(data, len);
        if (written != len) {
          ctx.writeFailed = true;
          return false;
        }
        ctx.downloaded += len;
        if (ctx.onProgress) ctx.onProgress(ctx.progressCtx, ctx.downloaded, ctx.expectedTotal);
        return true;
      },
      "", "", &httpStatus, SYNC_STORE.getAccessToken());

  // Close before any remove()/rename() on the same path below -- SdFat must
  // not rename/replace a path that still has an open FsFile (see
  // DESTRUCTOR_CLOSES_FILE's "close before reopen/delete" cases).
  tmpFile.flush();
  tmpFile.close();

  result.httpStatus = httpStatus;
  result.bytesDownloaded = ctx.downloaded;
  if (ctx.handshakeSampled) result.afterHandshake = ctx.afterHandshake;

  if (ctx.writeFailed) {
    LOG_ERR("BOOKDL", "SD write failed after %llu bytes for %s", (unsigned long long)ctx.downloaded,
            record.path.c_str());
    result.error = "sd_write_failed";
  } else if (ctx.cancelled) {
    LOG_DBG("BOOKDL", "Download of %s cancelled after %llu bytes", record.path.c_str(),
            (unsigned long long)ctx.downloaded);
    result.error = "cancelled";
  } else if (!fetchOk || httpStatus != 200) {
    LOG_ERR("BOOKDL", "Fetch failed for id=%s (ok=%d status=%d)", id.c_str(), fetchOk, httpStatus);
    result.error = "fetch_failed";
  } else if (ctx.downloaded == 0) {
    LOG_ERR("BOOKDL", "Empty body for id=%s", id.c_str());
    result.error = "empty_body";
  }

  if (!result.error.empty()) {
    Storage.remove(tmpPath.c_str());
    result.afterDownload = sampleHeap();
    return result;
  }

  // Atomic-rename-into-place -- ProgressFile.h's and SyncManifest.cpp's
  // pattern, applied to a whole book: the temp file is fully written and
  // closed before this, so an interrupted download leaves only the
  // discardable .part file, never a truncated book at the real path.
  Storage.remove(record.path.c_str());
  if (!Storage.rename(tmpPath.c_str(), record.path.c_str())) {
    LOG_ERR("BOOKDL", "Failed to rename %s into place", tmpPath.c_str());
    result.error = "rename_failed";
    result.afterDownload = sampleHeap();
    return result;
  }

  result.ok = true;
  if (!sync_manifest::markDownloaded(id)) {
    LOG_ERR("BOOKDL", "Downloaded %s but failed to flip its index flag", record.path.c_str());
    result.error = "index_update_failed";  // ok stays true -- see file header comment
  }

  result.afterDownload = sampleHeap();
  LOG_DBG("BOOKDL", "Downloaded %llu bytes to %s (heap: %u)", (unsigned long long)result.bytesDownloaded,
          record.path.c_str(), (unsigned)ESP.getFreeHeap());
  return result;
}

}  // namespace book_downloader
