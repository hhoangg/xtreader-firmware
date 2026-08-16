#include "ManifestIndexQuery.h"

#include <utility>

ManifestIndexPrefixScan::ManifestIndexPrefixScan(std::string prefix, MatchCallback onMatch, void* ctx)
    : prefix_(std::move(prefix)), onMatch_(onMatch), ctx_(ctx), reader_(&onRecordTrampoline, this) {}

bool ManifestIndexPrefixScan::feed(const uint8_t* data, size_t len) { return reader_.feed(data, len); }

bool ManifestIndexPrefixScan::onRecordTrampoline(void* ctx, const ManifestIndexRecord& record) {
  return static_cast<ManifestIndexPrefixScan*>(ctx)->onRecord(record);
}

bool ManifestIndexPrefixScan::onRecord(const ManifestIndexRecord& record) {
  const bool inPrefix = record.path.compare(0, prefix_.size(), prefix_) == 0;
  if (inPrefix) {
    enteredRange_ = true;
    if (onMatch_ && !onMatch_(ctx_, record)) return false;  // caller has enough matches already
    return true;
  }
  // Entries are sorted by path, so once a record no longer starts with
  // `prefix` after we've already seen some that did, every later record is
  // lexicographically past that contiguous run too -- nothing left to find.
  // (Not yet having entered the range just means the folder sorts later
  // still; keep scanning rather than guessing where it starts.)
  return !enteredRange_;
}

ManifestIndexIdLookup::ManifestIndexIdLookup(std::string id)
    : id_(std::move(id)), reader_(&onRecordTrampoline, this) {}

bool ManifestIndexIdLookup::feed(const uint8_t* data, size_t len) { return reader_.feed(data, len); }

bool ManifestIndexIdLookup::onRecordTrampoline(void* ctx, const ManifestIndexRecord& record) {
  return static_cast<ManifestIndexIdLookup*>(ctx)->onRecord(record);
}

bool ManifestIndexIdLookup::onRecord(const ManifestIndexRecord& record) {
  if (record.id != id_) return true;
  found_ = true;
  record_ = record;
  return false;  // stop -- found it, no need to scan the rest of the file
}
