#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// One row of the on-SD /.crosspoint/remote.idx index -- see SyncManifest.h
// for the file's overall shape and the three lookups it exists to support
// (prefix scan, id lookup, downloaded check).
//
// Same fields as ManifestEntry plus one this firmware owns, not the server:
// `downloaded`, set once a future downloader has actually pulled the book
// onto the SD card. Always false right after a sync -- this task builds
// only the manifest fetch, not downloading (see the task brief) -- but the
// column exists now so a later download step can flip it in place, in the
// same file format, without a migration.
struct ManifestIndexRecord {
  std::string id;
  std::string path;
  uint64_t sizeBytes = 0;
  std::string contentHash;
  uint64_t updatedAt = 0;
  bool downloaded = false;
};

// Formats one record as a single '|'-delimited line, trailing '\n' included
// (directly appendable to the index file):
//   bok_abc|/Kỹ năng/Đắc Nhân Tâm.epub|3014656|deadbeef...|1755300000|0
//
// '|' is safe as an unescaped delimiter here because the server sanitises
// every path segment to exclude it -- see crosspoint-sync docs/API.md:
// "path is already sanitised for FAT/exFAT -- no \ / : * ? " < > |" -- so a
// real path can never itself contain the delimiter, id/contentHash are
// server-generated opaque tokens that don't either, and no escaping scheme
// is needed for a value that structurally cannot occur.
std::string formatIndexLine(const ManifestIndexRecord& record);

// Inverse of formatIndexLine, for one already-line-buffered row (no
// trailing '\n' -- see LineChunker.h, which is what assembles index-file
// bytes read off SD into lines the same way it assembles manifest response
// bytes). Returns false -- this format's definition of a corrupt/malformed
// line -- if the line does not split into exactly six fields, id or path is
// empty, sizeBytes/updatedAt isn't a valid unsigned integer, or the
// `downloaded` field isn't exactly "0" or "1".
bool parseIndexLine(const char* line, size_t len, ManifestIndexRecord& out);

// --- Index file header ------------------------------------------------------
//
// The first INDEX_HEADER_LEN bytes of /.crosspoint/remote.idx, ahead of every
// record line formatIndexLine/parseIndexLine above handle. Carries two things
// a delta sync needs and a plain record line has nowhere to put: the on-disk
// layout's version (so a device flashed with a newer firmware that changes
// this format can tell an old-format file apart from one it wrote itself --
// see the task brief's "a format-version bump has to fall back to a full
// fetch") and the delta watermark (the newest server-side `updatedAt` this
// index reflects -- see SyncManifest.cpp's sync() for how that value is
// chosen and why it can never come from anything but the server's own
// entries, the device having no clock of its own).
//
// Fixed width, zero-padded, regardless of the values -- not just for a
// uniform parse, but so SyncManifest.cpp can write a placeholder header
// before it knows the final watermark (the sync isn't done yet), stream the
// rest of the file, and then seek(0) and overwrite just this line in place
// once the true value is known, the same seek-and-overwrite-in-place trick
// markDownloaded() already uses for a single flag byte -- see
// ManifestIndexDownloadedFlagLocator's class comment.
//
//   CPIDX|0000000001|00000000000000001755300000\n
//
// A missing file, a short read, a bad magic/delimiter, a non-digit in either
// field, or a version that doesn't match INDEX_FORMAT_VERSION all count as
// "no usable header" -- deliberately not distinguished further. Both a
// device upgraded from firmware that predates this header (its first line is
// a plain record line, not "CPIDX...") and mid-write corruption (a torn
// write can't happen in practice -- see SyncManifest.cpp's write-temp-
// then-rename -- but a card that silently corrupts a byte can) land here the
// same way: treated as "no index", which for the read-side queries in
// SyncManifest.h means "report nothing found" (already their contract for a
// literally-missing file) and for sync() means "do a full sync instead of a
// delta" -- recoverable without whatever is reading remote.idx ever needing
// to know an index file exists, let alone what shape it's in.
inline constexpr char INDEX_HEADER_MAGIC[] = "CPIDX";
inline constexpr size_t INDEX_HEADER_MAGIC_LEN = 5;
inline constexpr size_t INDEX_HEADER_VERSION_DIGITS = 10;    // fits any uint32_t
inline constexpr size_t INDEX_HEADER_WATERMARK_DIGITS = 20;  // fits any uint64_t
// magic + '|' + version digits + '|' + watermark digits + '\n'
inline constexpr size_t INDEX_HEADER_LEN =
    INDEX_HEADER_MAGIC_LEN + 1 + INDEX_HEADER_VERSION_DIGITS + 1 + INDEX_HEADER_WATERMARK_DIGITS + 1;

// Bump whenever remote.idx's on-disk layout changes (the header's own shape,
// or formatIndexLine/parseIndexLine's record shape). A reader that sees any
// other value treats the file as having no usable header at all -- see above.
inline constexpr uint32_t INDEX_FORMAT_VERSION = 1;

// Always exactly INDEX_HEADER_LEN bytes, trailing '\n' included.
std::string formatIndexHeader(uint32_t version, uint64_t watermark);

// `data` must be exactly INDEX_HEADER_LEN bytes (trailing '\n' included, same
// convention as formatIndexHeader's output -- unlike parseIndexLine, this one
// does take the newline, since a header's width is fixed and known up front
// rather than discovered by scanning for '\n'). Returns false -- version/
// watermark left untouched -- on any of the malformations described above.
// Does not check version against INDEX_FORMAT_VERSION; callers compare that
// themselves (see its own comment for why "wrong version" and "not a header
// at all" are handled identically one level up).
bool parseIndexHeader(const char* data, size_t len, uint32_t& version, uint64_t& watermark);
