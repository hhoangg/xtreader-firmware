#pragma once

#include <string>

namespace FsHelpers {

/**
 * Filesystem probes used by migrateLegacyCacheDir(). `ctx` is opaque, implementation-owned
 * state (e.g. a pointer to the storage layer, or to a test fixture).
 */
using PathExistsFn = bool (*)(void* ctx, const std::string& path);
using RenamePathFn = bool (*)(void* ctx, const std::string& oldPath, const std::string& newPath);

/**
 * Migration shim for the switch from path-hash to content-hash book cache directory names.
 *
 * If `newPath` does not exist but `legacyPath` does, renames legacyPath -> newPath so a book's
 * existing cache (including its saved reading position) survives the switch instead of being
 * silently orphaned. Idempotent: once `newPath` exists (because it was just migrated, or already
 * existed), subsequent calls are no-ops and never touch `legacyPath` again.
 *
 * Returns true if a migration rename was performed.
 */
bool migrateLegacyCacheDir(const std::string& newPath, const std::string& legacyPath, PathExistsFn exists,
                           RenamePathFn rename, void* ctx);

}  // namespace FsHelpers
