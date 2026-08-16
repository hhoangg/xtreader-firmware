#include "ManifestStreamParser.h"

#include "ManifestLineParser.h"

ManifestStreamParser::ManifestStreamParser(EntryCallback onEntry, TrailerCallback onTrailer, void* ctx)
    : onEntry_(onEntry), onTrailer_(onTrailer), ctx_(ctx), chunker_(&onLineTrampoline, this, MAX_LINE_LEN) {}

bool ManifestStreamParser::feed(const uint8_t* data, size_t len) { return chunker_.feed(data, len); }

bool ManifestStreamParser::onLineTrampoline(void* ctx, const char* line, size_t len) {
  return static_cast<ManifestStreamParser*>(ctx)->onLine(line, len);
}

bool ManifestStreamParser::onLine(const char* line, size_t len) {
  ManifestLine parsed;
  if (!parseManifestLine(line, len, parsed)) return false;  // malformed row -- abort the page

  if (parsed.kind == ManifestLine::Kind::TRAILER) {
    sawTrailer_ = true;
    trailer_ = parsed.trailer;
    if (onTrailer_) onTrailer_(ctx_, trailer_);
    return true;
  }
  // ENTRY
  if (onEntry_) return onEntry_(ctx_, parsed.entry);
  return true;
}
