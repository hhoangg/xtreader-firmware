#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Pure decision for "after a manifest sync, which books get inserted into
// the one recency list, and which get dropped from it?" -- factored out so
// the device-only caller (Task 3's post-sync glue) never has to reason
// about ordering, first-sync seeding, or the delete-on-server case itself.
// See test/recent_discovery and
// docs/superpowers/specs/2026-09-02-one-recency-list-design.md, the design
// this implements.
//
// The rules this encodes:
//  - A manifest record becomes a new entry when it is not already in the
//    list (current, by path), its file is not on the SD card, and it looks
//    like a book. New entries are ordered among themselves by updatedAt
//    descending, ties broken by path ascending, so the result is
//    deterministic.
//  - updatedAt is an opaque server value, comparable but never a date: the
//    device has no RTC, so it is only ever compared here, never subtracted
//    or formatted.
//  - The very first sync after pairing must insert nothing at all, even
//    with a full manifest -- otherwise a freshly paired device's entire
//    library floods the list as "new". The caller signals this with
//    firstSync; discovery starts from the second sync.
//  - A current entry with a non-empty remoteId whose id is no longer in the
//    manifest was deleted on the server, and is dropped -- otherwise it
//    would sit at the top of Home forever, undownloadable. This applies
//    regardless of firstSync.
//  - At most Input::maxInsert records are inserted in one sync, the newest
//    by updatedAt first.
namespace recent_discovery {

// One row from the server's manifest, already filtered by the caller to
// exclude tombstones. looksLikeBook is the caller's extension/type check --
// this file has no concept of what a book file looks like.
struct ManifestView {
  std::string id;
  std::string path;
  uint64_t updatedAt = 0;
  bool looksLikeBook = true;
};

// One entry from the current recency list, reduced to the two fields this
// decision needs.
struct ListEntry {
  std::string path;
  std::string remoteId;  // empty for a local entry
};

struct Input {
  std::vector<ManifestView> manifest;
  std::vector<ListEntry> current;
  std::vector<std::string> pathsOnDisk;
  // True on the sync immediately following pairing (or a re-pair): the
  // caller is expected to seed a marker on this call and never pass
  // firstSync=true again until the next unlink/pair cycle.
  bool firstSync = false;
  // How many records may be inserted in one sync. The newest by updatedAt
  // win; the rest are dropped and can return on a later sync. A library of
  // mostly-undownloaded books otherwise yields dozens of insertions in one
  // pass, each one an SD write of the whole list, for rows the list's remote
  // budget could never keep. Defaults to no cap: every caller is expected to
  // pass its own, and the only one passes the list's remote budget.
  size_t maxInsert = SIZE_MAX;
};

struct Result {
  // Most recent first (by updatedAt descending, ties by path ascending) --
  // the caller inserts these at the front of the list in this order.
  std::vector<ManifestView> insertFront;
  // remoteId values of current entries to remove: the server no longer has
  // them.
  std::vector<std::string> dropRemoteIds;
};

Result decide(const Input& in);

// True when a RecentBooksStore entry should be pruned as missing.
// hasRemoteId means the entry is a remote candidate, which by definition
// has no local file yet -- it has not arrived, it is not missing. See
// RecentBooksStore::isMissing(), the only caller: that decision cannot be
// host-tested directly because RecentBooksStore drags in Epub, Xtc,
// ArduinoJson and Arduino String, so the two-line rule lives here instead,
// pinned by tests, so deleting the exemption breaks a test rather than
// quietly emptying the reader's list.
bool shouldPrune(bool hasRemoteId, bool existsOnDisk);

// Running budget for capping the recency list. Walk the list once from most
// recent to oldest, calling keep() for each entry, and erase every entry it
// answers false for.
//
// Local and remote entries get separate budgets rather than sharing one, and
// that separation is the whole point: a remote entry is one manifest sync
// away from coming back, while a local entry carries reading history that is
// gone for good once the list is written over it. So a recoverable entry
// must never evict an unrecoverable one. Under a single shared cap, one sync
// against a large mostly-undownloaded library inserts enough remote entries
// at the front to push every book the reader has actually read off the end.
//
// A cleverer eviction order alone would not do it: with one shared cap, a
// list already holding localCap local entries would have to evict the remote
// entry it had just inserted. Two budgets is what makes both survivable.
struct TrimBudget {
  size_t localCap = 0;
  size_t remoteCap = 0;
  size_t localKept = 0;
  size_t remoteKept = 0;

  // True when this entry survives. Only kept entries are counted, so calling
  // it in list order evicts oldest-first within each kind.
  bool keep(bool isRemote);
};

}  // namespace recent_discovery
