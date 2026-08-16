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
