#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace FsHelpers {

/**
 * Callback used by partialContentHashFromReader() to pull bytes from an already-open file.
 *
 * Must read up to `length` bytes starting at absolute `offset` and write them into `buffer`,
 * returning the number of bytes actually read (0 on EOF or error). `ctx` is opaque,
 * implementation-owned state (e.g. a pointer to an open file handle); no allocation happens on
 * the caller side.
 */
using ChunkReadFn = size_t (*)(void* ctx, size_t offset, uint8_t* buffer, size_t length);

/**
 * Partial-content hash used to key on-disk book caches by file content rather than file path,
 * and (for EPUBs synced via KOReader) to match KOReader's own document-identification scheme.
 *
 * Reads at most 12 chunks of 1024 bytes at offsets `1024 << (2*i)` for i = -1 .. 10 (offset 0 for
 * i = -1), skipping any offset that falls at or beyond `fileSize`, and returns the 32-character
 * lowercase hex MD5 digest of the concatenated bytes actually read. Uses a 1 KB stack buffer; no
 * heap allocation.
 *
 * Returns "" if `fileSize` is 0, `readChunk` is null, or nothing could be read at all.
 */
std::string partialContentHashFromReader(size_t fileSize, ChunkReadFn readChunk, void* ctx);

/**
 * Convenience wrapper: opens `filePath` via the platform storage layer and computes its
 * partial-content hash. Returns "" if the file cannot be opened.
 *
 * Implemented in PartialContentHashFile.cpp, a firmware-only translation unit (it depends on
 * HalStorage, which needs FreeRTOS/SdFat and is not available on the host). Host tests exercise
 * partialContentHashFromReader() directly with a host-side reader instead.
 */
std::string calculatePartialContentHash(const std::string& filePath);

}  // namespace FsHelpers
