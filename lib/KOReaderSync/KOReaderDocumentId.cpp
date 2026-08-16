#include "KOReaderDocumentId.h"

#include <Logging.h>
#include <MD5Builder.h>
#include <PartialContentHash.h>

namespace {
// Extract filename from path (everything after last '/')
std::string getFilename(const std::string& path) {
  const size_t pos = path.rfind('/');
  if (pos == std::string::npos) {
    return path;
  }
  return path.substr(pos + 1);
}
}  // namespace

std::string KOReaderDocumentId::calculateFromFilename(const std::string& filePath) {
  const std::string filename = getFilename(filePath);
  if (filename.empty()) {
    return "";
  }

  MD5Builder md5;
  md5.begin();
  md5.add(filename.c_str());
  md5.calculate();

  std::string result = md5.toString().c_str();
  LOG_DBG("KODoc", "Filename hash: %s (from '%s')", result.c_str(), filename.c_str());
  return result;
}

std::string KOReaderDocumentId::calculate(const std::string& filePath) {
  // Delegate to the shared implementation in lib/FsHelpers so there is exactly one place that
  // computes the partial-content hash - it is also used to key CrossPoint's own book cache
  // directories.
  const std::string result = FsHelpers::calculatePartialContentHash(filePath);

  if (result.empty()) {
    LOG_DBG("KODoc", "Failed to compute partial content hash for: %s", filePath.c_str());
  } else {
    LOG_DBG("KODoc", "Hash calculated: %s (from %s)", result.c_str(), filePath.c_str());
  }

  return result;
}
