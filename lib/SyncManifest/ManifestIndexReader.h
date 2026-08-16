#pragma once

#include <cstddef>

#include "LineChunker.h"
#include "ManifestIndexFormat.h"

// Incrementally scans a /.crosspoint/remote.idx-formatted byte stream (fed
// in whatever chunks the caller has -- small SD reads on-device, a whole
// buffer at once in a host test) and calls onRecord for every successfully
// parsed row, in file order. Peak memory is one record plus one line
// buffer, independent of the index's size -- the same reasoning as
// ManifestStreamParser.h, applied to reading instead of writing. See
// ManifestIndexQuery.h for the prefix-scan and id-lookup built on top of
// this.
//
// A malformed line stops the scan as a real error (hasError() true) rather
// than being skipped over: a corrupt remote.idx means a bug or a torn write
// (see SyncManifest.cpp's write-temp-then-rename for why the latter
// shouldn't happen) -- worth surfacing, not silently working around.
class ManifestIndexReader {
 public:
  // Plain function pointer + context, not std::function (see CLAUDE.md's
  // "Template and std::function Bloat"). `ctx` is passed back unchanged; the
  // caller owns it and must keep it alive for as long as this reader is fed.
  // Return false to stop the scan early (id found, or a sorted prefix scan
  // has moved past the range it cares about) without that counting as an
  // error -- see stoppedEarly().
  using RecordCallback = bool (*)(void* ctx, const ManifestIndexRecord& record);

  ManifestIndexReader(RecordCallback onRecord, void* ctx, size_t maxLineLen = 2048);

  bool feed(const uint8_t* data, size_t len);

  bool hasError() const { return chunker_.hasError() && !stoppedEarly_; }
  bool stoppedEarly() const { return stoppedEarly_; }

 private:
  static bool onLineTrampoline(void* ctx, const char* line, size_t len);
  bool onLine(const char* line, size_t len);

  RecordCallback onRecord_;
  void* ctx_;
  LineChunker chunker_;
  bool stoppedEarly_ = false;
};
