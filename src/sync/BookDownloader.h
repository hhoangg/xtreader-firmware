#pragma once

#include <cstdint>
#include <string>

/**
 * Downloads one book from crosspoint-sync's GET /library/:id/file
 * (crosspoint-sync docs/API.md) onto the SD card, at the path the local
 * manifest index (SyncManifest.h) already has for it. The next layer above
 * SyncManifest -- that fetches the book list; this actually pulls a book's
 * bytes down. src/sync/DownloadQueue.h is the layer above this one, running
 * several of these back to back from a background task; no queueing or
 * FreeRTOS here, this is a single, synchronous, blocking call.
 *
 * Resume (HTTP Range / 206) is not implemented -- see download()'s comment
 * for why, and what would need to change to add it later without disturbing
 * this file's shape.
 */
namespace book_downloader {

// Free heap + largest allocatable block, in bytes -- same pair
// sync_manifest::SyncResult samples, at the same three points (before the
// request, after the response headers/first byte, after everything is
// done), so a device test can watch for the same "does the largest block
// move" fragmentation signal across a whole download.
struct HeapSample {
  uint32_t freeHeap = 0;
  uint32_t maxAllocHeap = 0;
};

struct DownloadResult {
  bool ok = false;
  // Empty on success. One of: "not_paired", "not_found" (id is not in the
  // local manifest index), "mkdir_failed", "open_tmp_failed", "fetch_failed"
  // (see httpStatus), "sd_write_failed", "empty_body", "cancelled",
  // "rename_failed". Can also be "index_update_failed" while ok is still
  // true -- the book's bytes landed safely, but SyncManifest.cpp's
  // markDownloaded() couldn't flip the index's flag; see download()'s
  // comment.
  std::string error;
  int httpStatus = -1;  // -1 if the request never got an HTTP response at all
  uint64_t bytesDownloaded = 0;
  std::string destPath;  // the manifest path the book was written to, once known

  HeapSample beforeRequest;
  HeapSample afterHandshake;
  HeapSample afterDownload;
};

// Called as bytes stream in; `total` is the manifest's sizeBytes for this
// book, not a value this module verifies against the real Content-Length --
// see download()'s comment. Plain function pointer + context, not
// std::function (see CLAUDE.md's "Template and std::function Bloat"). `ctx`
// is passed back unchanged; the caller owns it and must keep it alive for
// the duration of the call.
using ProgressCallback = void (*)(void* ctx, uint64_t downloaded, uint64_t total);

// Downloads the book identified by `id` (a SyncManifest id, e.g. "bok_...")
// to its indexed path on SD, creating any missing parent directories, via a
// temp-file-then-rename so an interrupted download can never leave a
// truncated file at the real path (src/activities/reader/ProgressFile.h's
// pattern, applied to a whole book). Flips the manifest index's `downloaded`
// flag on success.
//
// `cancelFlag`, if non-null, is polled between HTTP chunks -- the same
// mechanism HttpDownloader::downloadToFile's cancelFlag already uses. This
// call blocks for the whole book, so `cancelFlag` only does anything useful
// if something else (a different task) can flip it while this call is in
// flight; see DownloadQueue.cpp, which is the only caller today.
DownloadResult download(const std::string& id, ProgressCallback onProgress = nullptr, void* progressCtx = nullptr,
                        bool* cancelFlag = nullptr);

}  // namespace book_downloader
