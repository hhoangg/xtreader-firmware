#pragma once

#include <SyncTriggerPolicy.h>

#include <cstdint>
#include <string>

/**
 * Keeps /.sleep in step with the lock-screen wallpapers the server says are
 * assigned to this reader: fetches GET /wallpapers/manifest, downloads what
 * is assigned and missing, and deletes what this firmware previously
 * downloaded and the server no longer assigns.
 *
 * Nothing here draws anything. The display side already exists and is
 * untouched: src/activities/boot_sleep/SleepActivity.cpp picks a random
 * valid image out of /.sleep (falling back to /sleep) on every sleep, so
 * this module's entire job is to make the contents of that one directory
 * correct.
 *
 * The two halves are split the way src/sync/SyncManifest.cpp splits from
 * lib/SyncManifest: every decision -- which ids to fetch, which files to
 * delete, and above all which files are NOT this firmware's to touch --
 * lives in lib/WallpaperSync and is host-tested (test/wallpaper_sync). What
 * is left here is the device-only glue around HttpDownloader and HalStorage.
 */
namespace wallpaper_sync {

// Where SleepActivity looks first. Created lazily, only when there is
// actually a wallpaper to put in it: SleepActivity falls back to /sleep only
// when /.sleep yields no usable image, so creating it empty on a device with
// nothing assigned would be harmless but creating it eagerly is pointless.
constexpr char SLEEP_DIR[] = "/.sleep";

// Free heap + largest allocatable block, the same pair (and the same three
// sampling points) sync_manifest::SyncResult and book_downloader use, so a
// device test can watch one fragmentation signal across all three syncs.
struct HeapSample {
  uint32_t freeHeap = 0;
  uint32_t maxAllocHeap = 0;
};

struct SyncResult {
  bool ok = false;
  // Empty on a clean sync. One of: "not_paired", "fetch_failed" (see
  // httpStatus), "missing_trailer", "too_many_pages", "mkdir_failed", or
  // "low_space". "low_space" is the one that leaves `ok` true: the manifest
  // was fetched and the deletions were applied, only the downloads were
  // skipped, and `moreWorkPending` brings them back next boot. An individual
  // wallpaper failing to download sets neither -- see `failed`.
  std::string error;
  int httpStatus = -1;  // the last manifest page's status; -1 if none was ever received
  uint32_t pagesFetched = 0;
  uint32_t assignedCount = 0;  // alive rows the manifest listed, before any cap
  uint32_t downloaded = 0;
  uint32_t deleted = 0;
  uint32_t failed = 0;  // downloads that did not land; retried on the next sync
  // The per-sync download cap cut the list short (or a download failed), so
  // there is still work to do. HomeActivity leaves the boot counter due
  // rather than resetting it, so the next boot picks up where this left off.
  bool moreWorkPending = false;

  HeapSample beforeRequest;
  HeapSample afterHandshake;
  HeapSample afterSync;
};

// Reported once per finished wallpaper (downloaded or failed), so the caller
// can advance a progress bar. Plain function pointer + context, not
// std::function (see CLAUDE.md's "Template and std::function Bloat"); `ctx`
// is the caller's and must outlive the call.
using ProgressCallback = void (*)(void* ctx, uint32_t done, uint32_t total);

// Runs one whole reconciliation, blocking for its duration. Requires
// SYNC_STORE.isPaired() and a connected network; neither is brought up here
// (see HomeActivity::trySyncWallpapers, the only caller, and CMD:WALLPAPERSYNC
// in main.cpp for the serial-console trigger that exercises it directly).
//
// Always a FULL manifest, never a `since` delta. That is not an oversight:
// detaching a wallpaper from a device deletes the link row outright, leaving
// nothing for a delta to report (see apps/api/src/routes/wallpapers.ts's
// manifest comment), so a device that only ever asked for deltas would keep
// files it is no longer assigned. The set is a handful of rows, so a full
// manifest is cheap; the cadence, not the payload, is what keeps this
// affordable.
//
// timeoutMs bounds each manifest page's own fetch. Each wallpaper's file
// download gets sync_trigger::EXPLICIT_SYNC_TIMEOUT_MS instead -- a page of
// JSON and 96 KB of BMP do not belong on one budget.
SyncResult sync(uint32_t timeoutMs = sync_trigger::EXPLICIT_SYNC_TIMEOUT_MS, ProgressCallback onProgress = nullptr,
                void* progressCtx = nullptr);

}  // namespace wallpaper_sync
