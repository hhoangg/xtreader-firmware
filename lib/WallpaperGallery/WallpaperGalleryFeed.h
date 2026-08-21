#pragma once

#include <LineChunker.h>
#include <ManifestEntry.h>

#include <cstddef>
#include <cstdint>
#include <string>

// The wire half of the wallpaper gallery browser: the NDJSON bodies it reads
// and the request paths it reads them from.
//
// The format is the one lib/WallpaperSync/WallpaperManifest.h already
// documents -- one JSON object per line, then a trailer -- and the trailer is
// ManifestTrailer, shared verbatim, so ManifestPager drives pagination here
// exactly as it does for the book and wallpaper manifests.
//
// This is a second parser rather than an extension of wallpaper_manifest
// because the two want opposite things from the same bytes. Wallpaper sync
// deliberately drops `name`, `width`, `height` and the rest: it has no UI to
// spend heap on them and holds up to MAX_LOCAL rows at once. A gallery row is
// nothing BUT those display fields. Widening wallpaper_manifest::Entry would
// have put a display name and an owner name on every row of a sync that never
// draws either.
//
// Two endpoints, one row shape:
//   GET /wallpapers/gallery?sort=popular|recent&cursor=&limit=
//     id, name, width, height, sizeBytes, hasThumbnail, attachCount,
//     ownerName, attached
//   GET /wallpapers/manifest?cursor=&limit=
//     id, name, width, height, sizeBytes, contentHash, visibility, updatedAt
// Absent fields keep their defaults, so one parser covers both; see Sort::Mine
// in requestPath() for why the manifest endpoint is what the "on this device"
// tab reads.
namespace wallpaper_gallery {

// Display strings are truncated at parse time rather than stored whole. A tile
// is 134 px wide and gets two lines for the name and one shared line for the
// owner, so nothing near these bounds can be rendered anyway, and the cap is
// what keeps a page of rows from costing more heap than the screen it feeds.
// Both are byte counts applied on a UTF-8 boundary (see truncateUtf8()), so a
// Vietnamese name is cut between characters, never mid-sequence.
constexpr size_t MAX_NAME_BYTES = 64;
constexpr size_t MAX_OWNER_BYTES = 32;

struct Entry {
  std::string id;
  std::string name;
  std::string ownerName;
  uint64_t sizeBytes = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t attachCount = 0;
  bool hasThumbnail = false;
  // The server's "already on THIS device". The screen ORs this with what is
  // actually sitting in /.sleep, so a wallpaper attached moments ago shows its
  // corner mark without waiting for a refetch.
  bool attached = false;
  // Only ever set on a `since` delta, which this screen never asks for.
  // Parsed anyway so a tombstone is dropped rather than shown as a row.
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

// Cuts `text` to at most `maxBytes`, never inside a UTF-8 sequence.
std::string truncateUtf8(const char* text, size_t len, size_t maxBytes);

// Feeds raw response bytes -- exactly as HttpDownloader::fetchUrl's
// DataCallback hands them out, in ~1 KB pieces that can split mid-line and
// mid-UTF-8-sequence -- and emits one callback per row plus one for the
// trailer. Peak memory is one entry plus one line buffer.
class StreamParser {
 public:
  // Plain function pointers and a shared context, not std::function (see
  // AGENTS.md's "Template and std::function Bloat"). `ctx` is the caller's and
  // must outlive the parser. Return false from onEntry to abort the fetch.
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

  // A gallery row carries two free-text fields (a 120-character display name
  // and an owner name) on top of the manifest row's fixed ones, so it needs
  // more headroom than wallpaper_manifest's 1024 -- while still bounding a
  // hostile server's single line.
  static constexpr size_t MAX_LINE_LEN = 2048;
};

// Which listing a request is for. Mine reads GET /wallpapers/manifest -- the
// endpoint that already answers "what is assigned to this device", and the
// only one that can answer it without paging the whole gallery looking for
// `attached` rows. Popular and Recent are the same gallery under the two sort
// orders.
enum class Sort : uint8_t { Mine, Popular, Recent };

// Longest cursor this screen will echo back. The gallery cursor is opaque, so
// unlike the manifest's (which is a wallpaper id) its contents cannot be
// checked -- only its size, and that it survives percent-encoding.
constexpr size_t MAX_CURSOR_LEN = 256;

// Percent-encodes everything outside RFC 3986's unreserved set, so an opaque
// cursor cannot inject a second query parameter or a path segment. Returns ""
// for a cursor longer than MAX_CURSOR_LEN, which the caller treats as "stop
// paging" rather than as an empty cursor.
std::string encodeCursor(const std::string& cursor);

// The path and query to request, without the base URL. An empty cursor asks
// for the first page. A cursor that cannot be safely encoded (Popular/Recent)
// or is not a valid wallpaper id (Mine) is dropped, which ends pagination
// rather than sending something unchecked.
std::string requestPath(Sort sort, const std::string& cursor, uint32_t limit);

}  // namespace wallpaper_gallery
