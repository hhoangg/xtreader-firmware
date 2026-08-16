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
