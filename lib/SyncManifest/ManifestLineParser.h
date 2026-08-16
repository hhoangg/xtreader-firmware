#pragma once

#include <cstddef>

#include "ManifestEntry.h"

// What one already-line-buffered row of GET /library/manifest's
// newline-delimited JSON body parsed to -- either a book entry or the page's
// trailer line (see crosspoint-sync docs/API.md, "GET /library/manifest").
struct ManifestLine {
  enum class Kind : uint8_t { INVALID, ENTRY, TRAILER };
  Kind kind = Kind::INVALID;
  ManifestEntry entry;
  ManifestTrailer trailer;
};

// Parses one complete line (no trailing '\n' -- callers assemble that with
// LineChunker first; see ManifestStreamParser.h for the streaming version
// that does both together). Dependency-free (built on the project's own
// StreamingJsonParser, like lib/DevicePairing/DevicePairingProtocol.h), so
// this is host-testable without a device.
//
// Returns false, with out.kind left at INVALID, if the line is not valid
// JSON, or is a JSON object that is neither a valid entry (must have both
// "id" and "path") nor a valid trailer (must have "done").
bool parseManifestLine(const char* line, size_t len, ManifestLine& out);
