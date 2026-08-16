#pragma once

#include <cstddef>

#include "LineChunker.h"
#include "ManifestEntry.h"

// Feeds raw GET /library/manifest response bytes -- exactly as
// HttpDownloader::fetchUrl's DataCallback hands them out, in ~1 KB pieces
// that can split anywhere, including mid-line and mid-UTF-8-sequence -- and
// turns them into one callback per parsed book entry plus one for the page's
// trailer. See LineChunker.h's class comment for why a raw chunk boundary
// can never corrupt a Vietnamese path even when it lands mid-character.
//
// Peak memory is one entry plus one line buffer, independent of how many
// books the library holds or how many chunks/lines this instance has
// already seen: nothing here accumulates across lines. It is the caller's
// job to persist each entry (e.g. append it to the on-SD index) before the
// next line's callback reuses it -- see SyncManifest.cpp.
class ManifestStreamParser {
 public:
  // Plain function pointers + a shared context, not std::function (see
  // CLAUDE.md's "Template and std::function Bloat"). Both callbacks travel
  // with the same `ctx`, which the caller owns and must keep alive for as
  // long as this parser is fed.
  // Return false to abort the whole fetch (e.g. the caller's SD write failed).
  using EntryCallback = bool (*)(void* ctx, const ManifestEntry& entry);
  using TrailerCallback = void (*)(void* ctx, const ManifestTrailer& trailer);

  ManifestStreamParser(EntryCallback onEntry, TrailerCallback onTrailer, void* ctx);

  // Feeds the next chunk of raw response bytes for the current page. Returns
  // false on a malformed line, an oversized line, or onEntry aborting --
  // once false, stop feeding and inspect hasError()/hasTrailer().
  bool feed(const uint8_t* data, size_t len);

  bool hasTrailer() const { return sawTrailer_; }
  const ManifestTrailer& trailer() const { return trailer_; }
  bool hasError() const { return chunker_.hasError(); }

 private:
  static bool onLineTrampoline(void* ctx, const char* line, size_t len);
  bool onLine(const char* line, size_t len);

  EntryCallback onEntry_;
  TrailerCallback onTrailer_;
  void* ctx_;
  LineChunker chunker_;
  bool sawTrailer_ = false;
  ManifestTrailer trailer_;

  // Generous over any real id/path/hash line (docs/API.md's example paths
  // run a few dozen bytes even with Vietnamese diacritics) while still
  // bounding a hostile/broken server's single line -- see LineChunker.h.
  static constexpr size_t MAX_LINE_LEN = 2048;
};
