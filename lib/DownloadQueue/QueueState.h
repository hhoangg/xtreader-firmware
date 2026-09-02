#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Pure bounded-FIFO state machine behind the download queue -- no FreeRTOS,
// no network, no SD access, so this is fully host-testable (see
// test/download_queue_state). src/sync/DownloadQueue.cpp is the thin
// device-only wrapper: a mutex around one of these plus the worker task that
// actually calls book_downloader::download() and drives the transitions
// below in response, the same "pure core vs. device glue" split
// lib/SyncManifest and SyncManifest.cpp use for the manifest index.
namespace download_queue {

// Ten is comfortably more than a multi-select browsing session queues in one
// go, while keeping Snapshot (see DownloadQueue.h) small enough to return by
// value with no heap: MAX_QUEUE * sizeof(QueueItem), two short strings per
// item, is a few hundred bytes on the stack, not tens of KB.
constexpr size_t MAX_QUEUE = 10;

enum class ItemStatus : uint8_t { Pending, Downloading, Done, Failed, Cancelled };

struct QueueItem {
  std::string id;    // manifest-stable id (ManifestIndexRecord::id)
  std::string path;  // display path, captured from the manifest at enqueue time
  ItemStatus status = ItemStatus::Pending;
  std::string error;  // set only once status is Failed
  uint64_t downloadedBytes = 0;
  uint64_t totalBytes = 0;
};

class QueueState {
 public:
  enum class EnqueueResult { Ok, Full, AlreadyQueued };

  EnqueueResult enqueue(const std::string& id, const std::string& path, uint64_t totalBytes);

  bool empty() const { return count_ == 0; }
  size_t size() const { return count_; }
  bool contains(const std::string& id) const;

  // The item the worker should download next -- nullptr if empty.
  const QueueItem* front() const;

  // Flips the front item to Downloading once the worker actually starts the
  // HTTP request (idempotent; no-op if the queue is empty or `id` is not the
  // current front -- a race with cancelAll()/enqueue() from another task).
  void markDownloading(const std::string& id);

  // Updates the front item's progress counters from the worker's progress
  // callback. Same no-op-on-mismatch guard as markDownloading: a cancel can
  // race ahead of one last progress tick from the book that was in flight.
  void updateProgress(const std::string& id, uint64_t downloadedBytes, uint64_t totalBytes);

  // Removes the front item once the worker's download attempt for it has
  // concluded, successfully or not -- a failure still advances the queue,
  // it never halts it. No-op (does not touch any other item) if the queue is
  // empty or `id` is not the current front, so a result for a book that
  // cancelAll() already dropped can never corrupt whatever is queued now.
  void finish(const std::string& id, bool ok, const std::string& error);

  // Empties the whole queue immediately, pending items and whatever is mid-
  // download alike. The in-flight download itself is not stopped by this
  // call -- DownloadQueue.cpp pairs it with its own cancel flag that
  // book_downloader::download() polls between HTTP chunks; this method only
  // ever touches the state machine. Safe to call on an empty queue.
  void cancelAll();

  // Fixed-size copy for UI polling -- no heap. Returns the number of items
  // copied (min(size(), maxOut)).
  size_t copyItemsTo(QueueItem* out, size_t maxOut) const;

  // The outcome of the most recent finish() call -- what makes a failure
  // visible instead of silent, per the task brief: finish() pops the item
  // off the live queue (a failure must advance, not block, the rest of the
  // queue), so this is the only place a UI can still read what happened to
  // it. Overwritten by the next finish(); cancelAll() leaves it alone (a
  // cancel is not a completion, and the UI that just cancelled already knows
  // why the queue emptied).
  struct LastResult {
    bool hasResult = false;
    std::string id;
    std::string path;
    bool ok = false;
    std::string error;
  };
  const LastResult& lastResult() const { return lastResult_; }

  // Change-detector for a UI that has no callback to hang off: the queue is
  // poll-only, so a screen showing per-book status (FileBrowserActivity's
  // placeholder rows) has to ask "did anything change?" on every loop pass.
  // Doing that with copyItemsTo()/snapshot() would copy MAX_QUEUE items of
  // std::string every tick; comparing these two counters instead is a pair of
  // integer loads.
  //
  // generation() bumps on any membership or status change -- enqueue, the
  // flip to Downloading, finish, a cancel that actually emptied something --
  // but deliberately NOT on updateProgress(): that fires once per HTTP chunk
  // and no row renders a percentage, so it would repaint an e-ink screen
  // hundreds of times per book for no visible difference. A call that turns
  // out to be a no-op (a duplicate/overflowing enqueue, a transition for an
  // id that is no longer the front) leaves it alone, so the poller does not
  // rebuild on a race it cannot see.
  //
  // completions() bumps only on a successful finish(), which is the stronger
  // signal: a book that finished is a real file on SD now, so the reader of
  // this counter must re-list the directory, not just re-derive status text.
  // A failure and a cancel both leave it untouched -- neither created a file.
  //
  // Both are monotonic and never reset; consumers cache the last value they
  // saw and compare for inequality, so wraparound is harmless.
  uint32_t generation() const { return generation_; }
  uint32_t completions() const { return completions_; }

 private:
  QueueItem items_[MAX_QUEUE];
  size_t count_ = 0;
  LastResult lastResult_;
  uint32_t generation_ = 0;
  uint32_t completions_ = 0;
};

}  // namespace download_queue
