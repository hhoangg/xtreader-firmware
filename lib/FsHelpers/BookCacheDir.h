#pragma once

#include <string>

namespace FsHelpers {

/**
 * Resolve the on-disk cache directory for a book, keyed by file content rather than file path so
 * renaming or moving the book file does not orphan its cache (saved reading position, layout
 * cache, etc).
 *
 * `prefix` is the cache-type prefix used elsewhere to recognise book cache dirs (see
 * isBookCacheDirectoryName() in src/util/BookCacheUtils.cpp), e.g. "epub_", "txt_", "xtc_".
 *
 * If the file's content cannot be hashed (e.g. it can't be opened), falls back to the legacy
 * path-hash naming scheme so callers still get a usable cache dir.
 *
 * If a directory from the legacy path-hash scheme exists for this book but the new content-hash
 * directory does not, migrates it (renames it) so existing users don't lose their cache when this
 * scheme changes. See BookCacheMigration.h.
 *
 * Firmware-only: depends on HalStorage. Lives in BookCacheDir.cpp, which is not part of the host
 * test target.
 */
std::string resolveBookCacheDir(const std::string& cacheDir, const std::string& prefix, const std::string& filePath);

}  // namespace FsHelpers
