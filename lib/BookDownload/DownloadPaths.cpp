#include "DownloadPaths.h"

namespace book_download_paths {

std::string tempPathFor(const std::string& finalPath) { return finalPath + ".part"; }

std::string parentDirOf(const std::string& path) {
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash == std::string::npos) return "";
  if (lastSlash == 0) return "/";  // finalPath is directly under SD root
  return path.substr(0, lastSlash);
}

}  // namespace book_download_paths
