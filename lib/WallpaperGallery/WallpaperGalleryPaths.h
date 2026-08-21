#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Naming and cache rules for the wallpaper gallery browser's thumbnails.
// Pure string work, no SD and no network, so it is host-testable
// (test/wallpaper_gallery) the same way lib/WallpaperSync/WallpaperPaths.h is.
//
// Two rules matter here, and they are the same two WallpaperPaths.h exists
// for -- restated because the directory is different and the failure modes
// are not:
//
//  1. A cached thumbnail must be impossible to confuse with a wallpaper the
//     reader copied on by hand. That is why these files do NOT live in
//     /.sleep. A 134x223 thumbnail dropped in there would be picked up by
//     src/activities/boot_sleep/SleepActivity.cpp and drawn as a lock screen,
//     and wallpaper_reconcile::plan() would see a name it does not recognise
//     and leave it there forever. They live under /.crosspoint instead, which
//     AGENTS.md already reserves as this firmware's private cache root, in
//     their own subdirectory so clearing them never touches a book cache.
//
//  2. A hostile or buggy server id must not be able to steer a write (or a
//     delete) out of that directory. isValidId() is wallpaper_paths::isValidId
//     verbatim -- the same URL-safe alphabet and the same length bound -- and
//     classifyFileName() applies the same round-trip rule: a name is only ours
//     if thumbNameForId() would have produced it byte-for-byte.
namespace wallpaper_gallery_paths {

// Cache root. Created lazily, on the first thumbnail actually downloaded.
constexpr char THUMB_DIR[] = "/.crosspoint/wpthumb";

// Reserved filename shape for a cached thumbnail. Distinct from
// wallpaper_paths::MANAGED_PREFIX ("cpw_") so that even if the two
// directories were ever pointed at each other, neither module would claim the
// other's files.
constexpr char THUMB_PREFIX[] = "wpt_";
constexpr char THUMB_SUFFIX[] = ".bmp";
// The suffix a download streams into before being renamed into place, same
// discipline (and same reasoning) as wallpaper_paths::TEMP_SUFFIX: a cut-off
// download can only ever leave a discardable ".part", never a truncated .bmp
// that the grid would then try to draw.
constexpr char TEMP_SUFFIX[] = ".part";

// The single scratch file the full-size preview streams into. One fixed name,
// overwritten by every preview and removed when the screen exits, so the
// 96 KB preview cost is bounded at exactly one file no matter how many
// wallpapers get previewed in a session.
constexpr char PREVIEW_NAME[] = "wpv_preview.bmp";

// How many thumbnails the cache is allowed to hold. A thumbnail is a 134x223
// 2-bit BMP, ~8 KB (packages/contract/src/wallpaper.ts's bmp2BitByteLength for
// those dimensions), so 60 is ~480 KB -- ten screens of six, which covers a
// normal browse session end to end, and negligible next to the books on the
// same card. Past that, planCacheEviction() drops entries that are not on the
// screen the reader is looking at.
constexpr size_t MAX_CACHED_THUMBS = 60;

enum class FileKind : uint8_t {
  // Somebody else's file. Never written, never deleted, never counted.
  Unmanaged,
  // A finished thumbnail this screen downloaded.
  Thumb,
  // A ".part" left behind by a download that was cut off. Safe to delete
  // unconditionally -- nothing ever draws one.
  ThumbTemp,
  // The full-size preview scratch file (or its ".part"). Bounded at one file
  // and reclaimed on exit, so it is never an eviction candidate.
  Preview,
};

// True only for an id this screen is willing to build a filename from. Exactly
// wallpaper_paths::isValidId() -- shared rather than re-derived, so the
// gallery and the sync can never disagree about which ids are safe.
bool isValidId(const std::string& id);

// The canonical cache filename for `id`, or "" if isValidId() rejects it.
std::string thumbNameForId(const std::string& id);

// The ".part" name `fileName` is streamed into first.
std::string tempNameFor(const std::string& fileName);

// Which of the four kinds `fileName` is (a bare filename, not a path).
// Thumb/ThumbTemp are returned only when the name round-trips exactly through
// thumbNameForId(), so a case variant or any other near-miss stays Unmanaged
// and therefore undeletable.
FileKind classifyFileName(const std::string& fileName);

// The server id behind a Thumb filename, or "" for anything else.
std::string idFromThumbName(const std::string& fileName);

// dir + "/" + fileName, with a single separator. Delegates to
// wallpaper_paths::joinPath().
std::string joinPath(const std::string& dir, const std::string& fileName);

// Which cached files to delete, given the cache directory's raw listing and
// the ids currently on screen.
//
// Every ".part" is dropped unconditionally. Beyond that, the cache is trimmed
// to `maxEntries` finished thumbnails by discarding, in listing order, the
// ones whose id is not in `keepIds`. The on-screen page is therefore never
// evicted out from under the render that is about to draw it, and the reader's
// own files (Unmanaged) and the preview scratch are never candidates at all.
//
// Listing order, not recency: the X4 has no clock (see AGENTS.md), so a file's
// timestamp carries no information and an LRU policy would be a lie. Order is
// whatever the FAT directory hands back, which is stable for an unchanged
// directory -- good enough for a cache whose only failure mode is one extra
// download.
std::vector<std::string> planCacheEviction(const std::vector<std::string>& localNames,
                                           const std::vector<std::string>& keepIds,
                                           size_t maxEntries = MAX_CACHED_THUMBS);

}  // namespace wallpaper_gallery_paths
