#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "ManifestIndexFormat.h"
#include "ManifestIndexReader.h"

// The one primitive both a "delete a single book from the local manifest
// index" fix-up and a full delta-sync merge build on: stream an existing
// /.crosspoint/remote.idx-formatted index past (fed via feed(), one small
// chunk at a time -- see ManifestIndexReader.h, which this is built on) and
// report, through onRecord, the index it becomes once a batch of changes is
// applied -- any record whose id is in `removeIds` dropped (a stale copy an
// update replaces, or a tombstone), and every record in `upserts` spliced in
// at its correct sorted-by-path position as the old index streams past.
//
// Peak memory is O(len(removeIds) + len(upserts)) plus ManifestIndexReader's
// own one-line buffer -- never O(index size), which is only ever seen one SD
// read at a time. A single-book removal (SyncManifest.cpp's removeFromIndex,
// FileBrowserActivity's "Delete everywhere" fix-up) is just this primitive
// with a one-id removeIds and an empty upserts; a whole delta merge
// (SyncManifest.cpp's sync()) is the same primitive with removeIds/upserts
// both bounded by how much the library changed since the last sync, not by
// how large the library is -- the same reasoning ManifestPager's own
// page-count cap already rests on.
//
// This class does no I/O itself (host-testable without SD access -- see
// test/sync_manifest_index); SyncManifest.cpp is the device-only glue that
// reads the old index off SD, feeds it here, and writes onRecord's output to
// /.crosspoint/remote.idx.tmp before renaming it into place, same pattern
// sync() already uses for a full sync.
class ManifestIndexMerge {
 public:
  using RecordCallback = bool (*)(void* ctx, const ManifestIndexRecord& record);

  // removeIds need not be pre-sorted (sorted internally at construction --
  // O(delta size), not O(index size)). upserts MUST already be sorted by
  // path ascending -- the server's own manifest ordering guarantee for one
  // delta response (crosspoint-sync docs/API.md: "ordered by path") --
  // and normally shares every id with some entry in removeIds too: an
  // updated record is a stale-copy removal (wherever in the old index it
  // used to sort -- a rename can move it) plus a fresh insert at its new
  // sorted position, not an in-place edit. A plain tombstone belongs in
  // removeIds only; a brand new id belongs in upserts only (removeIds
  // matching nothing is a harmless no-op). See SyncManifest.cpp's
  // buildDeltaMerge() for how one manifest delta response is split into
  // these two lists.
  ManifestIndexMerge(std::vector<std::string> removeIds, std::vector<ManifestIndexRecord> upserts,
                     RecordCallback onRecord, void* ctx);

  bool feed(const uint8_t* data, size_t len);

  // Flushes every upsert not yet emitted -- the ones sorting after every
  // record the old index had (including all of them, for a brand new or
  // empty old index). Call exactly once, after the old index has been fully
  // fed (or never fed at all, for a from-scratch build). Returns false if a
  // previous feed() already failed.
  bool finish();

  bool hasError() const { return failed_; }

 private:
  static bool onOldRecordTrampoline(void* ctx, const ManifestIndexRecord& record);
  bool onOldRecord(const ManifestIndexRecord& record);
  bool flushUpsertsBefore(const std::string& path, bool hasBound);
  bool emit(const ManifestIndexRecord& record);

  std::vector<std::string> removeIds_;
  std::vector<ManifestIndexRecord> upserts_;
  size_t nextUpsert_ = 0;
  RecordCallback onRecord_;
  void* ctx_;
  ManifestIndexReader reader_;
  bool failed_ = false;
};
