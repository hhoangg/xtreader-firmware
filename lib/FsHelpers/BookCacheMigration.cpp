#include "BookCacheMigration.h"

namespace FsHelpers {

bool migrateLegacyCacheDir(const std::string& newPath, const std::string& legacyPath, const PathExistsFn exists,
                           const RenamePathFn rename, void* ctx) {
  if (exists == nullptr || rename == nullptr) {
    return false;
  }

  // Already migrated (or never needed migrating) - never touch legacyPath again.
  if (exists(ctx, newPath)) {
    return false;
  }

  if (!exists(ctx, legacyPath)) {
    return false;
  }

  return rename(ctx, legacyPath, newPath);
}

}  // namespace FsHelpers
