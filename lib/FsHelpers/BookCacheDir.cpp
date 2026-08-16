// Firmware-only translation unit: depends on HalStorage, so it is not part of the host test
// target. The portable pieces it composes (partialContentHashFromReader, migrateLegacyCacheDir)
// are covered by host tests under test/book_cache_key/.
#include "BookCacheDir.h"

#include <HalStorage.h>
#include <Logging.h>

#include "BookCacheMigration.h"
#include "PartialContentHash.h"

namespace FsHelpers {

namespace {

bool halExists(void* /*ctx*/, const std::string& path) { return Storage.exists(path.c_str()); }

bool halRename(void* /*ctx*/, const std::string& oldPath, const std::string& newPath) {
  return Storage.rename(oldPath.c_str(), newPath.c_str());
}

}  // namespace

std::string resolveBookCacheDir(const std::string& cacheDir, const std::string& prefix, const std::string& filePath) {
  const std::string contentHash = calculatePartialContentHash(filePath);

  // Legacy-only: CrossPoint used to key the cache dir by a hash of the file *path*. Keep this
  // computation around solely to detect and migrate pre-existing directories from that scheme -
  // it must not be used for anything else, since renaming/moving the book file changes it.
  const std::string legacyPath = cacheDir + "/" + prefix + std::to_string(std::hash<std::string>{}(filePath));

  if (contentHash.empty()) {
    // Could not open the file to hash its content (e.g. a transient storage error). Fall back to
    // the legacy path-hash scheme so callers still get a usable cache dir instead of none at all;
    // it just won't survive a future rename.
    LOG_ERR("FsHelpers", "Could not compute content hash for %s, falling back to legacy path hash", filePath.c_str());
    return legacyPath;
  }

  const std::string newPath = cacheDir + "/" + prefix + contentHash;
  migrateLegacyCacheDir(newPath, legacyPath, halExists, halRename, nullptr);
  return newPath;
}

}  // namespace FsHelpers
