#include "RecentDiscovery.h"

#include <algorithm>

namespace recent_discovery {

namespace {

bool pathInList(const std::vector<ListEntry>& list, const std::string& path) {
  for (const ListEntry& entry : list) {
    if (entry.path == path) return true;
  }
  return false;
}

bool pathInList(const std::vector<std::string>& paths, const std::string& path) {
  for (const std::string& p : paths) {
    if (p == path) return true;
  }
  return false;
}

bool idInManifest(const std::vector<ManifestView>& manifest, const std::string& id) {
  for (const ManifestView& record : manifest) {
    if (record.id == id) return true;
  }
  return false;
}

}  // namespace

Result decide(const Input& in) {
  Result result;

  // Removal applies regardless of firstSync: a remote entry can only exist
  // in `current` after a prior discovery, and firstSync is only true
  // immediately after pairing/unlinking, when current holds no remote
  // entries yet -- but the rule itself does not need that assumption to be
  // correct.
  for (const ListEntry& entry : in.current) {
    if (entry.remoteId.empty()) continue;  // local entry, not discovery's concern
    if (!idInManifest(in.manifest, entry.remoteId)) {
      result.dropRemoteIds.push_back(entry.remoteId);
    }
  }

  // A freshly paired device must not pull its whole library in as "new" --
  // see RecentDiscovery.h. Discovery starts from the second sync.
  if (in.firstSync) return result;

  std::vector<ManifestView> toInsert;
  toInsert.reserve(in.manifest.size());
  for (const ManifestView& record : in.manifest) {
    if (!record.looksLikeBook) continue;
    if (pathInList(in.current, record.path)) continue;
    if (pathInList(in.pathsOnDisk, record.path)) continue;
    toInsert.push_back(record);
  }

  // updatedAt is opaque and only ever compared, never interpreted as a
  // date -- see RecentDiscovery.h. Descending so the most recently updated
  // book leads; path ascending as the tiebreak keeps the result
  // deterministic when two records tie.
  std::sort(toInsert.begin(), toInsert.end(), [](const ManifestView& a, const ManifestView& b) {
    if (a.updatedAt != b.updatedAt) return a.updatedAt > b.updatedAt;
    return a.path < b.path;
  });

  // Sorted first, so the cap keeps the newest rather than whichever the
  // index scan happened to reach.
  if (toInsert.size() > in.maxInsert) {
    toInsert.resize(in.maxInsert);
  }

  result.insertFront = std::move(toInsert);
  return result;
}

bool shouldPrune(bool hasRemoteId, bool existsOnDisk) {
  if (hasRemoteId) return false;
  return !existsOnDisk;
}

bool TrimBudget::keep(const bool isRemote) {
  size_t& kept = isRemote ? remoteKept : localKept;
  const size_t cap = isRemote ? remoteCap : localCap;
  if (kept >= cap) return false;
  kept++;
  return true;
}

}  // namespace recent_discovery
