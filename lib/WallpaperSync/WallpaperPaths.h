#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Naming rules for the lock-screen wallpapers this device downloads into
// /.sleep. Pure string work, no SD or network access, so this is
// host-testable (see test/wallpaper_sync) the same way
// lib/BookDownload/DownloadPaths.h splits from BookDownloader.cpp's
// device-only glue.
//
// The whole point of this file is that /.sleep is a SHARED directory: the
// reader has always been able to drop their own .bmp files there by hand,
// and src/activities/boot_sleep/SleepActivity.cpp draws whatever it finds.
// Wallpaper sync therefore reconciles by set comparison against the server's
// manifest, which means it deletes files -- and deleting a file the reader
// copied over themselves would be data loss with no undo on a device that has
// no trash can.
//
// The guarantee, in one sentence: a file is only ever a deletion candidate if
// its name is byte-for-byte the canonical name fileNameForId() itself would
// have produced for some server wallpaper id, or that exact name plus the
// ".part" suffix tempNameFor() adds. Everything else -- every file the reader
// named -- is classified Unmanaged and is invisible to the reconciler.
// See classifyFileName() for the four rules that enforce it.
namespace wallpaper_paths {

// Reserved filename prefix for server-managed wallpapers. A reader who wants
// their own picture to survive a sync simply has to not start its filename
// with this; nothing else about /.sleep is reserved.
constexpr char MANAGED_PREFIX[] = "cpw_";
constexpr char MANAGED_SUFFIX[] = ".bmp";
// The ".part" a download streams into before being renamed into place, same
// suffix and same reasoning as book_download_paths::tempPathFor(): it must
// never be mistaken for a finished file, and SleepActivity's own extension
// check (FsHelpers::hasBmpExtension) skips it for free.
constexpr char TEMP_SUFFIX[] = ".part";

// Longest server id accepted. Server ids are `<prefix>_<base64url of 12
// random bytes>` (apps/api/src/lib/crypto.ts's generateId), i.e. 20 bytes
// today; the bound is generous over that while still keeping a managed
// filename far short of the 255-byte FAT long-name limit. Nothing here
// hardcodes the "wlp" prefix -- the contract types the id as a plain string
// (packages/contract/src/wallpaper.ts), so only its shape is relied on.
constexpr size_t MAX_ID_LEN = 48;

enum class FileKind : uint8_t {
  // Somebody else's file: a wallpaper the reader copied on by hand, a
  // stray README, anything at all. Never downloaded, never deleted, never
  // counted. The default for everything that fails any rule below.
  Unmanaged,
  // A finished wallpaper this firmware downloaded.
  Managed,
  // A ".part" left behind by a download that was cut off mid-stream (a power
  // cut, a flat battery). Safe to delete unconditionally -- it is by
  // definition not a file SleepActivity will ever draw.
  ManagedTemp,
};

// True only for an id this firmware would be willing to build a filename
// from: non-empty, at most MAX_ID_LEN bytes, and made only of the URL-safe
// base64 alphabet plus '_' and '-' that generateId() can emit. Rejecting
// anything else is what keeps a hostile or buggy server from steering a
// write (or a delete) at a path of its choosing via '/' or "..".
bool isValidId(const std::string& id);

// The canonical local filename for `id`, or "" if isValidId() rejects it.
std::string fileNameForId(const std::string& id);

// The ".part" name `fileName` is streamed into first.
std::string tempNameFor(const std::string& fileName);

// Which of the three kinds `fileName` is (a bare filename, not a path).
// Managed/ManagedTemp are returned only when the name round-trips exactly
// through fileNameForId(), so a case variant ("CPW_x.BMP") or any other
// near-miss stays Unmanaged and therefore undeletable.
FileKind classifyFileName(const std::string& fileName);

// The server id behind a Managed filename, or "" for anything else
// (ManagedTemp included -- a half-written file's id is of no use to the
// reconciler, which only wants to delete it).
std::string idFromFileName(const std::string& fileName);

// dir + "/" + fileName, with a single separator regardless of whether `dir`
// already ends in one.
std::string joinPath(const std::string& dir, const std::string& fileName);

}  // namespace wallpaper_paths
