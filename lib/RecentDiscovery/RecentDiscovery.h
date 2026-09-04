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
//  - A manifest record becomes a new entry when its updatedAt is beyond the
//    caller's discoveredWatermark, its file is not on the SD card, and it
//    looks like a book. New entries are ordered among themselves by
//    updatedAt descending, ties broken by path ascending, so the result is
//    deterministic.
//  - Novelty is deliberately NOT "absent from the list". The list is capped
//    far below the manifest (Input::maxInsert), so a record evicted by one
//    sync is again absent on the next -- "absent from the list" would
//    reinsert it forever, reshuffling the list every boot with no new
//    information behind any of it. Whether a record is already in the list
//    (pathInList) is kept only as a cheap in-memory guard against inserting
//    the same path twice in one pass, e.g. if a caller passes a stale mark.
//  - updatedAt is an opaque server value, comparable but never a date: the
//    device has no RTC, so it is only ever compared here, never subtracted
//    or formatted.
//  - Result::newWatermark is the highest updatedAt seen across every
//    book-like manifest record this call was given, not just the ones it
//    inserted, and never lower than Input::discoveredWatermark. It must
//    include records excluded from insertion (already in the list, already
//    on disk) too: such a record's updatedAt was what let it in on some
//    earlier sync, and if this call ignored it, the record would look
//    "new" again the moment the list evicts it. Ordinary callers persist
//    this value and pass it back in as the next call's discoveredWatermark.
//  - When more than maxInsert genuinely new records arrive between two
//    syncs, this is a burst, not a backlog: only the newest maxInsert of
//    them are inserted, and the mark advances past the whole batch anyway
//    -- deliberately, not an oversight. The rest are now below the mark and
//    stay there permanently; they never appear here again on any later
//    sync. Home draws a handful of rows and the list keeps a handful of
//    remote entries, so a batch bigger than the cap was never going to fit
//    on Home either way, and draining it gradually would only change which
//    five the reader sees while costing several boots to finish. Those
//    records remain reachable through the file browser and the download
//    queue; if a "why didn't my book show up on Home" report ever traces
//    here, this is why, not a bug.
//  - The very first sync after pairing must insert nothing at all, even
//    with a full manifest -- otherwise a freshly paired device's entire
//    library floods the list as "new". The caller signals this with
//    firstSync; discovery starts from the second sync. It still computes
//    and returns newWatermark, seeded from that first manifest, so the
//    second sync does not treat the whole pre-existing library as new.
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
  // The high-water mark from the last call this device acted on: a record
  // is a candidate for insertion only when its updatedAt is beyond this.
  // 0 (the default) treats everything as beyond the mark -- correct only
  // for a device that has never persisted one, which is exactly the state
  // a fresh NVS read comes back with.
  uint64_t discoveredWatermark = 0;
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
  // The value the caller should persist as the next call's
  // Input::discoveredWatermark. See the header comment above for why this
  // covers every book-like manifest record, not just insertFront, and is
  // never lower than the mark that came in.
  uint64_t newWatermark = 0;
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
