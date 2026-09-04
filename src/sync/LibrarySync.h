#pragma once

#include <cstdint>

/**
 * Runs HomeActivity's automatic library sync (Wi-Fi bring-up, manifest
 * fetch, piggybacked heartbeat) on its own FreeRTOS task instead of on the
 * render task.
 *
 * Today that whole sequence runs inside HomeActivity::render(), which holds
 * ActivityManager's rendering mutex for the entire call -- a captive portal
 * or a slow manifest page then leaves the device unresponsive behind a
 * full-screen popup for up to HOME_SYNC_TIMEOUT_MS, unable to redraw or take
 * button input, even though nothing about the sync itself needs the
 * rendering mutex. Moving it to a background task (same lazy-create,
 * self-delete-when-idle lifecycle as src/sync/DownloadQueue.h) frees the
 * render task to keep servicing input and navigation while the sync is in
 * flight; HomeActivity polls status() instead of blocking on the result.
 *
 * WORKER_STACK_BYTES matches DownloadQueue.cpp's and RemoteProgressCheck.cpp's
 * 12288: this task runs the identical TLS+HTTP+SD combination that measured
 * 4096 bytes crashed outright on hardware, at the first byte of the first
 * network read. That number was earned there, not assumed here.
 *
 * Status is polled, not pushed: the UI has no mechanism for a background
 * task to trigger a partial screen refresh (drawing is single-threaded,
 * e-ink refreshes are slow, and this module must not know any Activity
 * exists). A caller reads status() once per render pass and decides for
 * itself what, if anything, changed.
 */
namespace library_sync {

enum class Phase : uint8_t { Idle, ConnectingWifi, Syncing };

// Heap-free, no std::string members -- safe to copy out of the worker's
// mutex under any caller's stack, from any task.
struct Status {
  Phase phase = Phase::Idle;
  uint32_t generation = 0;  // bumped under the same lock as phase, on every phase change
  bool lastSyncOk = false;
  bool lastSyncRan = false;  // a sync completed since boot
};

// Checked by start() before it creates the worker task -- return false to
// refuse the run outright (e.g. a book is open and the read-session heap
// budget cannot afford a TLS session). Plain function pointer, not
// std::function, matching download_queue::SafetyCheck; pass nullptr to
// clear it (always safe to run).
using SafetyCheck = bool (*)();
void setSafetyCheck(SafetyCheck fn);

// Starts the background sync if nothing is already running and the safety
// check (if any) allows it. Never blocks the caller. Returns false if
// already running or refused; true does not mean a sync will actually run --
// the usual once-per-boot/paired/Wi-Fi gates (sync_trigger::shouldAutoSync(),
// shouldAttemptLibraryWifiConnect()) still apply once the task starts, same
// as before this module existed.
bool start();

// Heap-free snapshot for UI polling -- no allocation, safe from any task.
Status status();

}  // namespace library_sync
