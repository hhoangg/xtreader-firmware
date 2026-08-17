#pragma once

#include <cstdint>
#include <string>

#include "ManifestIndexFormat.h"
#include "ManifestIndexQuery.h"
#include "SyncTriggerPolicy.h"

/**
 * Fetches GET /library/manifest (crosspoint-sync docs/API.md) and persists
 * it to the SD card as a local, sorted, line-oriented index -- the next
 * layer above device pairing (SyncCredentialStore/DevicePairingProtocol),
 * fetching the book list. No downloading and no file-browser UI here; see
 * SyncManifest.cpp's top-of-file comment for the full rationale and the
 * decisions this task leaves open.
 *
 * Everything that can be host-tested (NDJSON parsing across chunk/UTF-8
 * boundaries, pagination bookkeeping, the index file format, and the three
 * read queries below) lives in lib/SyncManifest and has no Arduino/ESP-IDF
 * dependency -- see test/sync_manifest_parser and test/sync_manifest_index.
 * This file is the thin, device-only glue that drives HttpDownloader and
 * HalStorage around that tested core, mirroring how
 * src/activities/settings/SyncPairingActivity.cpp drives
 * lib/DevicePairing's protocol/poller classes.
 */
namespace sync_manifest {

// Where the local index lives, following README.md's ".crosspoint cache
// directory" convention. INDEX_TMP_PATH is written first and renamed over
// INDEX_PATH only once a full sync completes -- see sync()'s doc comment.
constexpr char INDEX_PATH[] = "/.crosspoint/remote.idx";
constexpr char INDEX_TMP_PATH[] = "/.crosspoint/remote.idx.tmp";

// Free heap + largest allocatable block, in bytes -- the task brief's
// "log free heap and largest block" pair, sampled at the three points it
// asks for (see SyncResult below). ESP.getFreeHeap()/getMaxAllocHeap().
struct HeapSample {
  uint32_t freeHeap = 0;
  uint32_t maxAllocHeap = 0;
};

struct SyncResult {
  bool ok = false;
  std::string error;  // empty on success; a short machine-readable reason otherwise (see sync()'s .cpp)
  uint32_t pagesFetched = 0;
  // How many manifest entry lines the server sent this sync. In delta mode this counts every delta
  // row received (updates, brand new books, and tombstones combined), not the net change in the
  // index's size -- see ManifestIndexMerge for what actually happens to each one.
  uint32_t entriesWritten = 0;
  uint32_t totalCount = 0;  // the server's totalCount, as of the last trailer seen
  // Whether this sync fetched `since` a prior watermark (merged onto the existing index) rather than
  // the whole library (see sync()'s doc comment for when each happens).
  bool deltaSync = false;

  HeapSample beforeRequest;   // before Wi-Fi/TLS/anything -- the sync's starting point
  HeapSample afterHandshake;  // inside the first page's first response-body callback (TLS + headers done)
  HeapSample afterLastPage;   // after the whole sync (all pages, index written and renamed into place)
};

// Fetches the manifest from the currently paired account and replaces
// INDEX_PATH with the result. Requires SYNC_STORE.isPaired() and a connected
// network; neither is brought up here -- see CMD:MANIFESTSYNC in main.cpp,
// which does both, for how this is exercised today ahead of any UI trigger.
//
// Full or delta, decided automatically from whatever is already on SD: if
// INDEX_PATH has a valid, current-format-version header (see
// ManifestIndexFormat.h), its watermark is sent as `since` and the response
// -- which may include tombstones -- is merged onto the existing index
// in place (see ManifestIndexMerge). Otherwise (never synced, an
// old-firmware index with no header, or a corrupt/truncated one)
// this fetches the whole library, same as always. A delta merge that finds
// the existing index itself unreadable mid-merge (corrupt, not just
// missing) automatically retries once as a full sync -- see SyncManifest.cpp
// -- rather than leaving the device with no working index and no way for
// its reader to know why.
//
// timeoutMs bounds each page's own fetch (a multi-page sync can pay it more
// than once) -- defaults to SyncTriggerPolicy.h's EXPLICIT_SYNC_TIMEOUT_MS,
// right for something the reader asked for directly (Sync Now); the
// automatic caller (HomeActivity) passes AUTO_SYNC_TIMEOUT_MS instead so a
// captive portal or black-holed server cannot stall the render task behind
// it -- see that header for why.
SyncResult sync(uint32_t timeoutMs = sync_trigger::EXPLICIT_SYNC_TIMEOUT_MS);

// --- Read-side queries over the on-SD index --------------------------------
// All three read INDEX_PATH in small fixed-size chunks, never the whole
// file at once -- same "peak memory independent of library size" reasoning
// as sync() itself, applied to reading (see ManifestIndexReader.h). Safe to
// call even if a sync has never run: all three then report "nothing found"
// rather than an error, so a caller doesn't need to check for the index's
// existence separately.

// Calls onMatch, in sorted order, for every entry whose path starts with
// `folderPrefix` -- how the (future) file browser is meant to render a
// folder: scan the sorted index rather than build a tree. Returns false
// only on a genuine read/parse error (a missing index or zero matches both
// return true). `ctx` is passed back to onMatch unchanged; the caller owns
// it and must keep it alive for the duration of this call.
bool listByPrefix(const std::string& folderPrefix, ManifestIndexPrefixScan::MatchCallback onMatch, void* ctx);

// Looks up a single entry by its stable id -- how a rename is told apart
// from a new book (crosspoint-sync docs/API.md: "id is stable across
// renames and moves"). Returns true and fills `out` only if found.
bool findById(const std::string& id, ManifestIndexRecord& out);

// Looks up a manifest id by its exact local path -- the reverse of
// findById(), for a book that is already a plain local file (loadFiles()'s
// normal directory scan, not a placeholder row: once a book is downloaded it
// has no fileRemoteId of its own, see FileBrowserActivity.h). Built on
// listByPrefix(path, ...): `path` is a leaf file path, not a folder prefix,
// so the "starts with" scan is fed the full path and only a record whose
// path is exactly equal to it is accepted; every other record sharing that
// prefix (there shouldn't be any for a real file path) is skipped rather
// than matched. Used by FileBrowserActivity's force-delete option (only
// offered when a local file has a known manifest id) and by ReaderActivity's
// book-finished telemetry (POST /events/book-finished's bookId). Returns
// true and fills `outId` only if an exact match is found; false for "not
// synced", "no index yet", and "no matching entry" alike, same "nothing
// found isn't an error" contract as the read queries above.
bool findIdByPath(const std::string& path, std::string& outId);

// Convenience over findById(): true only if `id` exists in the index and
// its `downloaded` flag is set.
bool isDownloaded(const std::string& id);

// Flips `id`'s `downloaded` column to true, in place: a single-byte seek+
// write (see ManifestIndexDownloadedFlagLocator in lib/SyncManifest), not a
// rewrite of the index -- the column exists in the format for exactly this
// (see ManifestIndexRecord's comment). Called by src/sync/BookDownloader.cpp
// once a book's bytes are safely renamed into place on SD. Returns false if
// `id` is not in the index, the index has never been synced, or the write
// failed; the caller treats that as "the book downloaded fine, but the
// index couldn't be updated" (logged, not fatal to the download itself).
bool markDownloaded(const std::string& id);

// Drops `id`'s entry from the local index, in place -- built on
// ManifestIndexMerge (a one-id removeIds, no upserts), the same primitive a
// delta sync's merge uses for many ids at once. For when the server side of
// a delete has already happened (book_server_delete::deleteFromServer) but
// the local index still lists the book: without this, the next
// FileBrowserActivity render finds the id still in remote.idx and shows the
// just-deleted row as an "On server" placeholder -- the opposite of what
// just happened -- until a full or delta resync eventually clears it. Safe
// to call for an id that turns out not to be present (no-op, true) or when
// there is no usable index at all (nothing to remove from, true) -- neither
// is a fault. Returns false only if a real read/parse error or an SD write
// failure left INDEX_TMP_PATH the sole trace of the attempt (removed before
// returning, so a retried removal or the next sync starts clean).
bool removeFromIndex(const std::string& id);

}  // namespace sync_manifest
