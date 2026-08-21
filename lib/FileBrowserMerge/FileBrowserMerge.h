#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ManifestIndexFormat.h"

/**
 * Merges one folder's local directory listing with matching /.crosspoint/remote.idx records into
 * the list FileBrowserActivity renders -- a remote-only book becomes a placeholder row, a
 * remote-only subfolder becomes a normal navigable folder, and a book present both locally and
 * remotely appears exactly once.
 *
 * Pure C++ (no Arduino/HalStorage dependency), so it is host-tested directly -- see
 * test/file_browser_merge. FileBrowserActivity.cpp is the thin device-only glue that feeds this
 * class from a real directory listing and a real sync_manifest::listByPrefix() scan, one record at
 * a time, mirroring how SyncManifest.cpp drives ManifestIndexPrefixScan/ManifestIndexIdLookup over
 * the same on-SD index. Nothing here ever holds more than one folder's worth of entries: the
 * whole-index-in-RAM cost this shape avoids is exactly what listByPrefix's streaming scan (see
 * SyncManifest.h) already exists to avoid on the read side.
 */
namespace file_browser_merge {

// True for a bare filename FileBrowserActivity::Mode::Books already shows locally
// (.epub/.xtc/.xtch/.txt/.md/.bmp/.png, case-insensitive). A small, deliberate duplicate of
// FsHelpers::hasXExtension()'s suffix checks: FsHelpers.h unconditionally includes Arduino's
// WString.h, which is not available when this file is built for the host test executable.
bool isRecognizedBookName(std::string_view name);

// One row of a merged folder listing.
struct MergedEntry {
  std::string name;      // bare name, as FileBrowserActivity::files stores it (trailing '/' for a directory)
  std::string remoteId;  // manifest id; empty means "local" (not a placeholder)
};

// Builds the merged listing for one folder. Construct with the folder's local directory listing
// (as FileBrowserActivity::loadFiles() already produces it), then feed every record a
// listByPrefix(folderPrefix) scan reports via addRemoteRecord(). The result is intentionally
// unsorted: FileBrowserActivity applies its own directories-first/natural sort
// (FsHelpers::sortFileList's ordering) across the combined result, so this class does not need to
// duplicate that ordering logic -- or its Arduino dependency -- to be useful.
class FolderMerge {
 public:
  // folderPrefix must end in '/' (e.g. "/" for the root, "/Notes/" for a subfolder) -- how a
  // record's immediate child name is derived below. localNames is copied in: one folder's worth of
  // entries, matching the "no whole-index-in-RAM" budget this class exists to respect.
  FolderMerge(std::string folderPrefix, std::vector<std::string> localNames);

  // Feeds one record from a listByPrefix(folderPrefix) scan. The scan may report records several
  // folders below folderPrefix (see ManifestIndexQuery.h's prefix scan, which matches on the raw
  // path string); such a record contributes only its immediate child folder name here, once --
  // later records under the same subfolder are no-ops -- never the book itself. A record landing
  // directly in this folder becomes a new placeholder entry only if: it isn't already marked
  // downloaded (record.downloaded -- see ManifestIndexRecord's comment: always false today, but
  // this stays correct once a downloader sets it), no local or already-added-remote entry has the
  // same name already (the "is this already on the device" decision -- see the class comment), and
  // it looks like a book FileBrowserActivity::Mode::Books would ever show.
  void addRemoteRecord(const ManifestIndexRecord& record);

  // Local entries first (their original order preserved), followed by every remote addition, in
  // encounter order. Safe to call any number of times before takeEntries().
  const std::vector<MergedEntry>& entries() const { return entries_; }

  // Move-extracts entries() for a one-shot caller (FileBrowserActivity.cpp) that wants to avoid a
  // second copy of every name string. Leaves this object's entries empty; every other member stays
  // valid, but the object is not meant to be reused afterwards.
  std::vector<MergedEntry> takeEntries() { return std::move(entries_); }

 private:
  bool hasEntry(const std::string& name) const;

  std::string prefix_;
  std::vector<MergedEntry> entries_;
  // Tracks the most recently added/seen remote subfolder name so consecutive records under the
  // same subfolder (guaranteed contiguous -- entries are scanned in sorted path order, see
  // ManifestIndexQuery.h) don't each re-scan entries_ for a duplicate.
  std::string lastRemoteFolder_;
  bool hasLastRemoteFolder_ = false;
};

}  // namespace file_browser_merge
