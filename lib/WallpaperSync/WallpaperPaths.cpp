#include "WallpaperPaths.h"

#include <cstring>

namespace wallpaper_paths {

namespace {

constexpr size_t PREFIX_LEN = sizeof(MANAGED_PREFIX) - 1;
constexpr size_t SUFFIX_LEN = sizeof(MANAGED_SUFFIX) - 1;
constexpr size_t TEMP_LEN = sizeof(TEMP_SUFFIX) - 1;

bool isIdChar(const char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool endsWith(const std::string& s, const char* suffix, const size_t suffixLen) {
  return s.size() >= suffixLen && memcmp(s.data() + s.size() - suffixLen, suffix, suffixLen) == 0;
}

// The one place the round-trip rule lives: strip the fixed prefix/suffix,
// then rebuild the name from what is left and demand it match exactly. Any
// name that is not already in canonical form fails here, which is what makes
// classifyFileName() safe to hand deletion decisions.
std::string managedIdOrEmpty(const std::string& fileName) {
  if (fileName.size() <= PREFIX_LEN + SUFFIX_LEN) return "";
  if (memcmp(fileName.data(), MANAGED_PREFIX, PREFIX_LEN) != 0) return "";
  if (!endsWith(fileName, MANAGED_SUFFIX, SUFFIX_LEN)) return "";

  const std::string id = fileName.substr(PREFIX_LEN, fileName.size() - PREFIX_LEN - SUFFIX_LEN);
  if (fileNameForId(id) != fileName) return "";
  return id;
}

}  // namespace

bool isValidId(const std::string& id) {
  if (id.empty() || id.size() > MAX_ID_LEN) return false;
  for (const char c : id) {
    if (!isIdChar(c)) return false;
  }
  return true;
}

std::string fileNameForId(const std::string& id) {
  if (!isValidId(id)) return "";
  std::string name;
  name.reserve(PREFIX_LEN + id.size() + SUFFIX_LEN);
  name += MANAGED_PREFIX;
  name += id;
  name += MANAGED_SUFFIX;
  return name;
}

std::string tempNameFor(const std::string& fileName) { return fileName + TEMP_SUFFIX; }

FileKind classifyFileName(const std::string& fileName) {
  if (!managedIdOrEmpty(fileName).empty()) return FileKind::Managed;
  if (endsWith(fileName, TEMP_SUFFIX, TEMP_LEN)) {
    const std::string base = fileName.substr(0, fileName.size() - TEMP_LEN);
    if (!managedIdOrEmpty(base).empty()) return FileKind::ManagedTemp;
  }
  return FileKind::Unmanaged;
}

std::string idFromFileName(const std::string& fileName) { return managedIdOrEmpty(fileName); }

std::string joinPath(const std::string& dir, const std::string& fileName) {
  if (dir.empty()) return fileName;
  if (dir.back() == '/') return dir + fileName;
  return dir + "/" + fileName;
}

}  // namespace wallpaper_paths
