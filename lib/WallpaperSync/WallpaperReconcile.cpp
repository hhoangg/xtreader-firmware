#include "WallpaperReconcile.h"

#include <algorithm>

namespace wallpaper_reconcile {

namespace {

bool contains(const std::vector<std::string>& sorted, const std::string& value) {
  return std::binary_search(sorted.begin(), sorted.end(), value);
}

}  // namespace

ReconcilePlan plan(const std::vector<std::string>& assignedIds, const std::vector<std::string>& localNames,
                   const Limits& limits) {
  ReconcilePlan out;

  // The set this device intends to end up holding: valid, de-duplicated, and
  // truncated to maxLocal in the order the server sent them.
  std::vector<std::string> keepOrdered;
  std::vector<std::string> keepSorted;
  keepOrdered.reserve(std::min(assignedIds.size(), limits.maxLocal));
  keepSorted.reserve(keepOrdered.capacity());

  for (const std::string& id : assignedIds) {
    if (!wallpaper_paths::isValidId(id)) continue;
    if (contains(keepSorted, id)) continue;  // duplicate row for the same wallpaper
    if (keepOrdered.size() >= limits.maxLocal) {
      out.droppedForLocalCap++;
      continue;
    }
    keepOrdered.push_back(id);
    keepSorted.insert(std::lower_bound(keepSorted.begin(), keepSorted.end(), id), id);
  }

  // What is already on the card, split by whose file it is. Unmanaged names
  // never reach either list, which is the entire safety property here.
  std::vector<std::string> presentSorted;
  presentSorted.reserve(localNames.size());
  for (const std::string& name : localNames) {
    switch (wallpaper_paths::classifyFileName(name)) {
      case wallpaper_paths::FileKind::Managed: {
        const std::string id = wallpaper_paths::idFromFileName(name);
        if (contains(keepSorted, id)) {
          presentSorted.insert(std::lower_bound(presentSorted.begin(), presentSorted.end(), id), id);
        } else {
          // Either detached on the server, soft-deleted, or pushed past
          // maxLocal by a bigger set than this device will hold.
          out.deleteNames.push_back(name);
        }
        break;
      }
      case wallpaper_paths::FileKind::ManagedTemp:
        // A download that never finished. Nothing draws it and nothing
        // resumes it -- see WallpaperSync.cpp on why there is no resume.
        out.deleteNames.push_back(name);
        break;
      case wallpaper_paths::FileKind::Unmanaged:
        break;
    }
  }

  for (const std::string& id : keepOrdered) {
    if (contains(presentSorted, id)) continue;
    if (out.downloadIds.size() >= limits.maxDownloadsPerSync) {
      out.moreWorkPending = true;
      break;
    }
    out.downloadIds.push_back(id);
  }

  return out;
}

}  // namespace wallpaper_reconcile
