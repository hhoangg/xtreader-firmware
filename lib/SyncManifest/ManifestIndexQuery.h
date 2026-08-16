#pragma once

#include <string>

#include "ManifestIndexFormat.h"
#include "ManifestIndexReader.h"

// Two read-only queries over a /.crosspoint/remote.idx-formatted byte
// stream, both incremental (see ManifestIndexReader.h) so a caller with the
// index on SD can feed it in small reads instead of loading the whole file
// into RAM -- see SyncManifest.h for why that matters and where the actual
// SD-reading loop lives (device-only; these two classes are pure and
// host-testable).

// Lists every entry whose path starts with `prefix` -- how the file browser
// will render a folder, per the task brief: scan the sorted index rather
// than build a tree. Because entries are written sorted by path (the server
// already sends them that way; SyncManifest.cpp's writer preserves it) and
// byte-order comparison of UTF-8 is the same as Unicode codepoint order,
// every match forms one contiguous run. This stops feeding (feed() returns
// false, hasError() still false -- a deliberate early stop, not a fault) as
// soon as a record's path moves lexicographically past that run, without
// needing to reach end of file. When a folder happens to be the last one in
// the index the scan instead runs to EOF, which is likewise not an error --
// just nothing more for feed() to give.
class ManifestIndexPrefixScan {
 public:
  // Plain function pointer + context, not std::function (see CLAUDE.md's
  // "Template and std::function Bloat"). `ctx` is passed back unchanged; the
  // caller owns it and must keep it alive for as long as this scan is fed.
  // Return false from onMatch to stop early once the caller has enough
  // (e.g. a folder listing that only needs the first N rows).
  using MatchCallback = bool (*)(void* ctx, const ManifestIndexRecord& record);

  ManifestIndexPrefixScan(std::string prefix, MatchCallback onMatch, void* ctx);

  bool feed(const uint8_t* data, size_t len);
  bool hasError() const { return reader_.hasError(); }

 private:
  static bool onRecordTrampoline(void* ctx, const ManifestIndexRecord& record);
  bool onRecord(const ManifestIndexRecord& record);

  std::string prefix_;
  MatchCallback onMatch_;
  void* ctx_;
  ManifestIndexReader reader_;
  bool enteredRange_ = false;
};

// Finds the single entry with the given stable `id` -- how a rename is told
// apart from a new book (see crosspoint-sync docs/API.md: "id is stable
// across renames and moves"). Unlike path, id is not sorted in this index,
// so this scans until found or end of file; found()/record() report the
// result once feed() returns (or has been called with the whole file).
class ManifestIndexIdLookup {
 public:
  explicit ManifestIndexIdLookup(std::string id);

  bool feed(const uint8_t* data, size_t len);
  bool hasError() const { return reader_.hasError(); }
  bool found() const { return found_; }
  const ManifestIndexRecord& record() const { return record_; }

 private:
  static bool onRecordTrampoline(void* ctx, const ManifestIndexRecord& record);
  bool onRecord(const ManifestIndexRecord& record);

  std::string id_;
  bool found_ = false;
  ManifestIndexRecord record_;
  ManifestIndexReader reader_;
};

// Finds the absolute byte offset of the `downloaded` flag character for a
// given id -- the single byte a downloader needs to seek to and overwrite
// with '1' once a book lands on SD, instead of rewriting the whole index
// (see ManifestIndexRecord's comment: the column exists for exactly this).
// Unlike ManifestIndexReader/ManifestIndexPrefixScan/ManifestIndexIdLookup
// above, this does not go through LineChunker: none of those track the
// absolute file offset a line started at, which is exactly what this class
// needs, so it does its own minimal line-splitting instead. Pure and
// host-tested (test/sync_manifest_index); the device-only seek+write lives
// in src/sync/SyncManifest.cpp's markDownloaded().
class ManifestIndexDownloadedFlagLocator {
 public:
  // capacity bounds a single line's length, same role and same default as
  // ManifestIndexReader's -- guards against an unbounded line (a corrupt
  // index with no '\n') growing this class's line buffer without limit.
  explicit ManifestIndexDownloadedFlagLocator(std::string id, size_t capacity = 2048);

  // Feed bytes in file order, starting at file offset 0, across as many
  // calls as the caller's read chunk size requires. Returns false once and
  // for all once the id has been found (nothing left to look for) or the
  // line buffer overflowed -- see hasError()/found() to tell those apart.
  bool feed(const uint8_t* data, size_t len);

  bool hasError() const { return overflowed_; }
  bool found() const { return found_; }
  // Valid only once found() is true: the absolute offset, from the start of
  // the byte stream fed to this scan, of the line's `downloaded` character
  // (its last character, immediately before the line's trailing '\n').
  size_t flagOffset() const { return flagOffset_; }

 private:
  std::string id_;
  std::string lineBuf_;
  size_t capacity_;
  size_t offset_ = 0;           // absolute offset of the next byte to be processed
  size_t lineStartOffset_ = 0;  // absolute offset where the current (buffered) line began
  bool found_ = false;
  bool overflowed_ = false;
  size_t flagOffset_ = 0;
};
