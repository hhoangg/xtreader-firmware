#include "QueueState.h"

namespace download_queue {

QueueState::EnqueueResult QueueState::enqueue(const std::string& id, const std::string& path, uint64_t totalBytes) {
  if (contains(id)) return EnqueueResult::AlreadyQueued;
  if (count_ >= MAX_QUEUE) return EnqueueResult::Full;

  QueueItem& item = items_[count_++];
  item = QueueItem{};
  item.id = id;
  item.path = path;
  item.totalBytes = totalBytes;
  return EnqueueResult::Ok;
}

bool QueueState::contains(const std::string& id) const {
  for (size_t i = 0; i < count_; i++) {
    if (items_[i].id == id) return true;
  }
  return false;
}

const QueueItem* QueueState::front() const { return count_ > 0 ? &items_[0] : nullptr; }

void QueueState::markDownloading(const std::string& id) {
  if (count_ == 0 || items_[0].id != id) return;
  items_[0].status = ItemStatus::Downloading;
}

void QueueState::updateProgress(const std::string& id, uint64_t downloadedBytes, uint64_t totalBytes) {
  if (count_ == 0 || items_[0].id != id) return;
  items_[0].downloadedBytes = downloadedBytes;
  items_[0].totalBytes = totalBytes;
}

void QueueState::finish(const std::string& id, bool ok, const std::string& error) {
  if (count_ == 0 || items_[0].id != id) return;
  lastResult_ = LastResult{true, id, items_[0].path, ok, error};
  // Shift left -- MAX_QUEUE is small (10), so this is a handful of moves,
  // not worth a circular-buffer index for the complexity it would add.
  for (size_t i = 1; i < count_; i++) {
    items_[i - 1] = std::move(items_[i]);
  }
  count_--;
}

void QueueState::cancelAll() { count_ = 0; }

size_t QueueState::copyItemsTo(QueueItem* out, size_t maxOut) const {
  const size_t n = count_ < maxOut ? count_ : maxOut;
  for (size_t i = 0; i < n; i++) {
    out[i] = items_[i];
  }
  return n;
}

}  // namespace download_queue
