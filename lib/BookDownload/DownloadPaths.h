#pragma once

#include <string>

// Pure path helpers behind the book downloader (src/sync/BookDownloader.cpp)
// -- no SD/network access, so these are host-testable (see
// test/book_download_paths) the same way lib/SyncManifest splits its parsing
// core from SyncManifest.cpp's device-only glue.
namespace book_download_paths {

// The temp file a book is streamed into before being renamed over
// `finalPath` on success, mirroring src/activities/reader/ProgressFile.h's
// crash-safe write-then-rename pattern applied to a whole book instead of
// progress.bin. ".part" rather than ".tmp": it must never collide with a
// real filename glob (FileBrowserActivity lists by extension), and reads
// unambiguously as "not a finished download" to anyone browsing the card
// directly with a file manager.
std::string tempPathFor(const std::string& finalPath);

// The directory that must exist before `finalPath` can be created: the
// substring up to (not including) the last '/'. Empty for a path with no
// directory component; "/" is returned as-is (already the root, nothing to
// create). The manifest's `path` field is always SD-absolute (starts with
// '/'), so callers pass that straight through.
std::string parentDirOf(const std::string& path);

}  // namespace book_download_paths
