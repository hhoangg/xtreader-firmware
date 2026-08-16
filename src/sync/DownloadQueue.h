#pragma once

#include <cstddef>
#include <string>

#include "QueueState.h"

/**
 * Runs several BookDownloader downloads one after another, strictly
 * sequential -- never two concurrent downloads, since two TLS sessions at
 * once on ~137 KB of free heap is how this device runs out of memory (see
 * the task brief). Picking one book downloads it; picking several in a row
 * enqueues them and they download in order, one at a time.
 *
 * A background FreeRTOS task (created lazily on the first enqueue(), self-
 * deleted once the queue drains) is what makes downloads interruptible: a
 * blocking, multi-second HTTP transfer run directly from the UI task would
 * freeze button input for its whole duration, so there would be no way to
 * read a cancel request while it is in flight. The task owns nothing but
 * this queue; all SD/network calls it makes go through the already
 * thread-safe HalStorage/HttpDownloader, same as any other task.
 *
 * The pure FIFO bookkeeping (order, bound, failure-advances-not-halts,
 * cancel-empties) lives in lib/DownloadQueue/QueueState.h/.cpp and is
 * host-tested there (test/download_queue_state); this is the thin
 * device-only wrapper: a mutex around one QueueState plus the worker task.
 */
namespace download_queue {

enum class EnqueueOutcome { Ok, Full, AlreadyQueued, NotFound, NotPaired };

// Checked before the worker starts (or resumes) the front item -- return
// false to make it pause, not cancel, until this returns true again. Wired
// from main.cpp so "sync from the library screen, never with a book open"
// (this device's ~50 KB free-while-reading heap budget makes a TLS session
// there risky, see docs/API.md's heap numbers) is enforced by the queue
// itself rather than by every future caller remembering to check. Plain
// function pointer, not std::function (see CLAUDE.md's "Template and
// std::function Bloat"); pass nullptr to clear it (always safe to run).
using SafetyCheck = bool (*)();
void setSafetyCheck(SafetyCheck fn);

// Looks the id up in the local manifest index (SyncManifest.h), and if
// found, adds it to the queue and starts/wakes the worker task.
EnqueueOutcome enqueue(const std::string& id);

// Empties the queue and signals whatever is currently downloading to abort
// at the next HTTP chunk boundary (the same cancelFlag mechanism
// HttpDownloader::downloadToFile already uses) -- "someone who queued ten
// books by mistake needs a way out that is not a reboot," per the task
// brief. Safe to call even if nothing is queued or running.
void cancelAll();

// Bounded, heap-free snapshot for UI polling -- no allocation, fixed
// MAX_QUEUE array, safe to call from any task.
struct Snapshot {
  QueueItem items[MAX_QUEUE];
  size_t count = 0;
  bool workerRunning = false;
  QueueState::LastResult lastResult;  // the most recent finished item, if any
};
Snapshot snapshot();

}  // namespace download_queue
