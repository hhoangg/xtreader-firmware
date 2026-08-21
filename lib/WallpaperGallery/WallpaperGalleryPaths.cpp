#include "WallpaperGalleryPaths.h"

#include <WallpaperPaths.h>

#include <algorithm>
#include <cstring>

namespace wallpaper_gallery_paths {

namespace {

constexpr size_t PREFIX_LEN = sizeof(THUMB_PREFIX) - 1;
constexpr size_t SUFFIX_LEN = sizeof(THUMB_SUFFIX) - 1;
constexpr size_t TEMP_LEN = sizeof(TEMP_SUFFIX) - 1;

bool endsWith(const std::string& s, const char* suffix, const size_t suffixLen) {
  return s.size() >= suffixLen && memcmp(s.data() + s.size() - suffixLen, suffix, suffixLen) == 0;
}

// The one place the round-trip rule lives: strip the fixed prefix/suffix, then
// rebuild the name from what is left and demand it match exactly.
std::string thumbIdOrEmpty(const std::string& fileName) {
  if (fileName.size() <= PREFIX_LEN + SUFFIX_LEN) return "";
  if (memcmp(fileName.data(), THUMB_PREFIX, PREFIX_LEN) != 0) return "";
  if (!endsWith(fileName, THUMB_SUFFIX, SUFFIX_LEN)) return "";

  const std::string id = fileName.substr(PREFIX_LEN, fileName.size() - PREFIX_LEN - SUFFIX_LEN);
  if (thumbNameForId(id) != fileName) return "";
  return id;
}

bool contains(const std::vector<std::string>& sorted, const std::string& value) {
  return std::binary_search(sorted.begin(), sorted.end(), value);
}

}  // namespace

bool isValidId(const std::string& id) { return wallpaper_paths::isValidId(id); }

std::string thumbNameForId(const std::string& id) {
  if (!isValidId(id)) return "";
  std::string name;
  name.reserve(PREFIX_LEN + id.size() + SUFFIX_LEN);
  name += THUMB_PREFIX;
  name += id;
  name += THUMB_SUFFIX;
  return name;
}

std::string tempNameFor(const std::string& fileName) { return fileName + TEMP_SUFFIX; }

FileKind classifyFileName(const std::string& fileName) {
  if (fileName == PREVIEW_NAME || fileName == tempNameFor(PREVIEW_NAME)) return FileKind::Preview;
  if (!thumbIdOrEmpty(fileName).empty()) return FileKind::Thumb;
  if (endsWith(fileName, TEMP_SUFFIX, TEMP_LEN)) {
    const std::string base = fileName.substr(0, fileName.size() - TEMP_LEN);
    if (!thumbIdOrEmpty(base).empty()) return FileKind::ThumbTemp;
  }
  return FileKind::Unmanaged;
}

std::string idFromThumbName(const std::string& fileName) { return thumbIdOrEmpty(fileName); }

std::string joinPath(const std::string& dir, const std::string& fileName) {
  return wallpaper_paths::joinPath(dir, fileName);
}

std::vector<std::string> planCacheEviction(const std::vector<std::string>& localNames,
                                           const std::vector<std::string>& keepIds, const size_t maxEntries) {
  std::vector<std::string> keepSorted;
  keepSorted.reserve(keepIds.size());
  for (const std::string& id : keepIds) {
    if (!isValidId(id)) continue;
    keepSorted.insert(std::lower_bound(keepSorted.begin(), keepSorted.end(), id), id);
  }

  std::vector<std::string> out;

  // Pass one: the finished thumbnails, split into the ones pinned by the
  // current page and the ones that are merely cached.
  std::vector<std::string> evictable;
  evictable.reserve(localNames.size());
  size_t thumbCount = 0;
  for (const std::string& name : localNames) {
    switch (classifyFileName(name)) {
      case FileKind::ThumbTemp:
        out.push_back(name);
        break;
      case FileKind::Thumb:
        thumbCount++;
        if (!contains(keepSorted, idFromThumbName(name))) evictable.push_back(name);
        break;
      case FileKind::Preview:
      case FileKind::Unmanaged:
        break;
    }
  }

  // Pass two: trim down to the bound, oldest-listed first. Pinned entries are
  // never touched, so a page bigger than maxEntries simply leaves the cache
  // over its bound rather than evicting what is about to be drawn.
  for (const std::string& name : evictable) {
    if (thumbCount <= maxEntries) break;
    out.push_back(name);
    thumbCount--;
  }

  return out;
}

}  // namespace wallpaper_gallery_paths
