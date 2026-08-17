#include "HeartbeatPayload.h"

#include <limits>

namespace heartbeat_payload {

uint64_t computeSdTotalBytes(const uint64_t totalBytes) {
  if (totalBytes == 0) return std::numeric_limits<uint64_t>::max();
  return totalBytes;
}

uint64_t computeSdFreeBytes(const uint64_t totalBytes, const uint64_t usedBytes) {
  if (totalBytes == 0 || usedBytes > totalBytes) return std::numeric_limits<uint64_t>::max();
  return totalBytes - usedBytes;
}

}  // namespace heartbeat_payload
