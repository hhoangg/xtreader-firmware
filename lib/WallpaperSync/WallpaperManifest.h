#pragma once

#include <LineChunker.h>
#include <ManifestEntry.h>

#include <cstddef>
#include <cstdint>
#include <string>

// GET /wallpapers/manifest's newline-delimited body, parsed the same way
// lib/SyncManifest parses GET /library/manifest -- deliberately, because the
// server made the two wire formats identical for exactly this reason (see
// apps/api/src/routes/wallpapers.ts). One row per wallpaper assigned to this
// reader, then a trailer:
//   {"id":"wlp_...","name":"Ha Long","width":480,"height":800,
//    "sizeBytes":96070,"contentHash":"...","visibility":"private",
//    "updatedAt":1755300000}
//   {"done":true,"nextCursor":null,"totalCount":7}
//
// Only the three fields reconciliation and downloading actually consume are
// kept: the id (which is also the local filename and, since the bytes are
// immutable, the content identity), the size (checked against what arrives,
// so a truncated body cannot be renamed into place), and the tombstone flag.
// `name`, `width`, `height`, `contentHash` and `visibility` are parsed past
// and dropped -- there is no gallery UI on the device to spend heap on them.
//
// The trailer is ManifestTrailer, shared verbatim with the book manifest, so
// ManifestPager drives pagination for both.
namespace wallpaper_manifest {

struct Entry {
  std::string id;
  uint64_t sizeBytes = 0;
  // Set only on a `since` delta response. This firmware always asks for the
  // full manifest (see WallpaperSync.cpp for why a delta cannot express a
  // detach), so in practice this never arrives; parsed anyway so a tombstone
  // is dropped rather than mistaken for an assignment.
  bool deleted = false;
};

struct Line {
  enum class Kind : uint8_t { INVALID, ENTRY, TRAILER };
  Kind kind = Kind::INVALID;
  Entry entry;
  ManifestTrailer trailer;
};

// Parses one complete line (no trailing '\n'). Returns false, leaving
// out.kind at INVALID, for anything that is neither a row carrying an "id"
// nor a trailer carrying "done".
bool parseLine(const char* line, size_t len, Line& out);

// Feeds raw response bytes -- exactly as HttpDownloader::fetchUrl's
// DataCallback hands them out, in ~1 KB pieces that can split mid-line and
// mid-UTF-8-sequence -- and emits one callback per row plus one for the
// trailer. Peak memory is one entry plus one line buffer regardless of how
// many wallpapers the account has; see LineChunker.h for why a chunk
// boundary can never corrupt a name's encoding.
class StreamParser {
 public:
  // Plain function pointers and a shared context, not std::function (see
  // CLAUDE.md's "Template and std::function Bloat"). `ctx` is the caller's
  // and must outlive the parser. Return false from onEntry to abort the
  // fetch.
  using EntryCallback = bool (*)(void* ctx, const Entry& entry);
  using TrailerCallback = void (*)(void* ctx, const ManifestTrailer& trailer);

  StreamParser(EntryCallback onEntry, TrailerCallback onTrailer, void* ctx);

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

  // A wallpaper row has no path, and its longest field is a 120-character
  // display name (packages/contract/src/wallpaper.ts's WallpaperName), so
  // these lines are far shorter than the book manifest's. 1024 is generous
  // over that while still bounding a hostile server's single line.
  static constexpr size_t MAX_LINE_LEN = 1024;
};

}  // namespace wallpaper_manifest
