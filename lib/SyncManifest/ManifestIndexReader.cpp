#include "ManifestIndexReader.h"

ManifestIndexReader::ManifestIndexReader(RecordCallback onRecord, void* ctx, size_t maxLineLen)
    : onRecord_(onRecord), ctx_(ctx), chunker_(&onLineTrampoline, this, maxLineLen) {}

bool ManifestIndexReader::feed(const uint8_t* data, size_t len) { return chunker_.feed(data, len); }

bool ManifestIndexReader::onLineTrampoline(void* ctx, const char* line, size_t len) {
  return static_cast<ManifestIndexReader*>(ctx)->onLine(line, len);
}

bool ManifestIndexReader::onLine(const char* line, size_t len) {
  ManifestIndexRecord record;
  if (!parseIndexLine(line, len, record)) return false;  // real error -- corrupt index line

  if (!onRecord_) return true;
  if (onRecord_(ctx_, record)) return true;

  stoppedEarly_ = true;
  return false;
}
