// Firmware-only translation unit: wires partialContentHashFromReader() up to HalStorage.
// Not compiled into the host test target (HalStorage needs FreeRTOS/SdFat); host tests exercise
// partialContentHashFromReader() directly with a host-side reader.
#include <HalStorage.h>

#include "PartialContentHash.h"

namespace FsHelpers {

namespace {

size_t readChunkFromHalFile(void* ctx, const size_t offset, uint8_t* buffer, const size_t length) {
  auto* file = static_cast<HalFile*>(ctx);
  if (!file->seekSet(offset)) {
    return 0;
  }
  const int bytesRead = file->read(buffer, length);
  return bytesRead > 0 ? static_cast<size_t>(bytesRead) : 0;
}

}  // namespace

std::string calculatePartialContentHash(const std::string& filePath) {
  HalFile file;
  if (!Storage.openFileForRead("FsHelpers", filePath, file)) {
    return "";
  }

  const size_t fileSize = file.fileSize();
  return partialContentHashFromReader(fileSize, readChunkFromHalFile, &file);
}

}  // namespace FsHelpers
