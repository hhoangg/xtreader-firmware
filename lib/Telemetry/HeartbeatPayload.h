#pragma once

#include <cstdint>

// Pure decision for the one non-obvious field in the heartbeat payload
// (src/sync/Telemetry.h's HeartbeatInfo): deriving sdFreeBytes from
// HalStorage::sdTotalBytes()/sdUsedBytes(). Factored out so it can be
// host-tested without ESP-IDF/Arduino (see test/heartbeat_payload), the same
// reasoning as lib/SyncManifest/SyncTriggerPolicy.h.
namespace heartbeat_payload {

// Passes totalBytes through, or UINT64_MAX (HeartbeatInfo's "omit this
// field" sentinel) when totalBytes == 0 -- SDCardManager::sdTotalBytes() is
// 0 until begin() has mounted the card, and reporting a 0-byte card would be
// read as "totally full" rather than "unknown".
uint64_t computeSdTotalBytes(uint64_t totalBytes);

// Returns totalBytes - usedBytes, or UINT64_MAX (the same "omit" sentinel)
// when the inputs can't be trusted: totalBytes == 0 means the card was never
// mounted, and usedBytes > totalBytes means the used-byte cache raced a
// mount/unmount and is stale. The server treats an omitted field as "leave
// the stored value alone" -- sending a wrong 0 would instead tell it the
// card is full, which is worse than saying nothing.
uint64_t computeSdFreeBytes(uint64_t totalBytes, uint64_t usedBytes);

}  // namespace heartbeat_payload
