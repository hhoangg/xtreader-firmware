#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "WallpaperPaths.h"

// The whole decision half of wallpaper sync: given what the server says is
// assigned to this reader and what is currently sitting in /.sleep, work out
// which files to fetch and which to delete. Pure -- no SD, no network, no
// Arduino -- so the rule that decides what gets deleted off somebody's card
// is host-tested (test/wallpaper_sync) rather than only ever exercised on
// hardware, mirroring how lib/SyncManifest keeps its parsing core testable
// away from src/sync/SyncManifest.cpp's glue.
namespace wallpaper_reconcile {

// How many server wallpapers this device is willing to keep on the card at
// once. 96,070 bytes each (the panel-sized 2-bit BMP -- see
// packages/contract/src/wallpaper.ts's bmp2BitByteLength), so 24 is ~2.2 MB:
// negligible next to the books on the same card, and comfortably more than
// CrossPointState::SLEEP_RECENT_COUNT (16), which is the point below which
// SleepActivity's "don't repeat a recent image" window would start starving
// itself of candidates. An account with more than this many wallpapers
// attached to one reader keeps the first MAX_LOCAL by id -- a stable subset,
// so the same ones stay put across syncs instead of churning the card.
constexpr size_t MAX_LOCAL = 24;

// How many wallpapers a single sync will download before stopping and
// leaving the rest for the next one. ~384 KB and a handful of seconds, all
// of it blocking the render task behind a popup (see
// HomeActivity::trySyncWallpapers) -- a bound on how long one visit to the
// library screen can be held up, not on the total, which MAX_LOCAL covers.
// The remainder is not lost: ReconcilePlan::moreWorkPending tells the caller
// to come back on the next boot rather than waiting out the normal cadence.
constexpr size_t MAX_DOWNLOADS_PER_SYNC = 4;

struct Limits {
  size_t maxLocal = MAX_LOCAL;
  size_t maxDownloadsPerSync = MAX_DOWNLOADS_PER_SYNC;
};

struct ReconcilePlan {
  // Server ids to fetch, in manifest order, at most maxDownloadsPerSync of them.
  std::vector<std::string> downloadIds;
  // Bare filenames (not paths) to delete from the wallpaper directory. Only
  // ever names wallpaper_paths::classifyFileName() calls Managed or
  // ManagedTemp -- see that header for the guarantee this rests on.
  std::vector<std::string> deleteNames;
  // Assigned wallpapers that are still missing after downloadIds is applied,
  // because the per-sync cap cut the list short. Not the same thing as
  // droppedForLocalCap below: this work is merely deferred, not refused.
  bool moreWorkPending = false;
  // Assigned wallpapers beyond maxLocal, which this device will never fetch.
  // Reported so the caller can log it; there is no UI for it.
  size_t droppedForLocalCap = 0;
};

// `assignedIds` is every alive wallpaper the manifest listed for this device,
// in the order the server sent them (ascending id -- the manifest's keyset
// order, so the kept subset is stable across syncs). Duplicates and ids
// wallpaper_paths::isValidId() rejects are dropped.
//
// `localNames` is the wallpaper directory's raw listing: every filename in
// it, the reader's own included. Names this module does not recognise as its
// own are counted for nothing and deleted never.
ReconcilePlan plan(const std::vector<std::string>& assignedIds, const std::vector<std::string>& localNames,
                   const Limits& limits = Limits{});

}  // namespace wallpaper_reconcile
