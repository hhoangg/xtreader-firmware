#include "ManifestIndexMerge.h"

#include <algorithm>
#include <utility>

ManifestIndexMerge::ManifestIndexMerge(std::vector<std::string> removeIds, std::vector<ManifestIndexRecord> upserts,
                                        RecordCallback onRecord, void* ctx)
    : removeIds_(std::move(removeIds)),
      upserts_(std::move(upserts)),
      onRecord_(onRecord),
      ctx_(ctx),
      reader_(&onOldRecordTrampoline, this) {
  std::sort(removeIds_.begin(), removeIds_.end());
}

bool ManifestIndexMerge::feed(const uint8_t* data, size_t len) {
  if (failed_) return false;
  if (reader_.feed(data, len)) return true;
  // Either a genuinely corrupt old-index line (reader_ caught it internally)
  // or onOldRecord() returned false because emit() already set failed_ below
  // -- either way, nothing further from this stream can be trusted.
  failed_ = true;
  return false;
}

bool ManifestIndexMerge::onOldRecordTrampoline(void* ctx, const ManifestIndexRecord& record) {
  return static_cast<ManifestIndexMerge*>(ctx)->onOldRecord(record);
}

bool ManifestIndexMerge::emit(const ManifestIndexRecord& record) {
  if (onRecord_ && !onRecord_(ctx_, record)) {
    failed_ = true;
    return false;
  }
  return true;
}

bool ManifestIndexMerge::flushUpsertsBefore(const std::string& path, const bool hasBound) {
  while (nextUpsert_ < upserts_.size() && (!hasBound || upserts_[nextUpsert_].path < path)) {
    if (!emit(upserts_[nextUpsert_])) return false;
    nextUpsert_++;
  }
  return true;
}

bool ManifestIndexMerge::onOldRecord(const ManifestIndexRecord& record) {
  // Sorted-by-path output requires every pending upsert that belongs before this old record to be
  // emitted first -- the old index streams past in path order, so this is the only point that ever
  // sees where a given upsert's path falls relative to it.
  if (!flushUpsertsBefore(record.path, /*hasBound=*/true)) return false;
  if (std::binary_search(removeIds_.begin(), removeIds_.end(), record.id)) return true;  // dropped
  return emit(record);
}

bool ManifestIndexMerge::finish() {
  if (failed_) return false;
  return flushUpsertsBefore("", /*hasBound=*/false);
}
