#include "WallpaperManifest.h"

#include <StreamingJsonParser.h>

#include <cstring>

namespace wallpaper_manifest {

namespace {

bool matches(const char* value, const size_t len, const char* literal) {
  return len == strlen(literal) && memcmp(value, literal, len) == 0;
}

uint64_t parseUint64Token(const char* value, const size_t len) {
  uint64_t result = 0;
  for (size_t i = 0; i < len; i++) {
    const char c = value[i];
    if (c < '0' || c > '9') break;
    result = result * 10 + static_cast<uint64_t>(c - '0');
  }
  return result;
}

// Every field either shape carries that this device consumes, at the line's
// top-level object (depth 1). One flat enum for both, as in
// ManifestLineParser.cpp: a line is only ever a row or a trailer, and their
// field names do not collide.
enum class Key : uint8_t {
  NONE,
  ID,
  SIZE_BYTES,
  DELETED,
  DONE,
  NEXT_CURSOR,
  TOTAL_COUNT,
};

struct Ctx {
  Line* out;
  Key lastKey = Key::NONE;
  uint8_t depth = 0;
  bool sawId = false;
  bool sawDone = false;
};

void onKey(void* ctxPtr, const char* key, const size_t len) {
  auto* ctx = static_cast<Ctx*>(ctxPtr);
  ctx->lastKey = Key::NONE;
  if (ctx->depth != 1) return;
  if (matches(key, len, "id"))
    ctx->lastKey = Key::ID;
  else if (matches(key, len, "sizeBytes"))
    ctx->lastKey = Key::SIZE_BYTES;
  else if (matches(key, len, "deleted"))
    ctx->lastKey = Key::DELETED;
  else if (matches(key, len, "done"))
    ctx->lastKey = Key::DONE;
  else if (matches(key, len, "nextCursor"))
    ctx->lastKey = Key::NEXT_CURSOR;
  else if (matches(key, len, "totalCount"))
    ctx->lastKey = Key::TOTAL_COUNT;
}

void onString(void* ctxPtr, const char* value, const size_t len) {
  auto* ctx = static_cast<Ctx*>(ctxPtr);
  switch (ctx->lastKey) {
    case Key::ID:
      ctx->out->entry.id.assign(value, len);
      ctx->sawId = true;
      break;
    case Key::NEXT_CURSOR:
      ctx->out->trailer.nextCursor.assign(value, len);
      ctx->out->trailer.hasNextCursor = true;
      break;
    default:
      break;
  }
  ctx->lastKey = Key::NONE;
}

void onNumber(void* ctxPtr, const char* value, const size_t len) {
  auto* ctx = static_cast<Ctx*>(ctxPtr);
  switch (ctx->lastKey) {
    case Key::SIZE_BYTES:
      ctx->out->entry.sizeBytes = parseUint64Token(value, len);
      break;
    case Key::TOTAL_COUNT:
      ctx->out->trailer.totalCount = static_cast<uint32_t>(parseUint64Token(value, len));
      break;
    default:
      break;
  }
  ctx->lastKey = Key::NONE;
}

void onBool(void* ctxPtr, const bool value) {
  auto* ctx = static_cast<Ctx*>(ctxPtr);
  if (ctx->lastKey == Key::DELETED) {
    ctx->out->entry.deleted = value;
  } else if (ctx->lastKey == Key::DONE) {
    ctx->out->trailer.done = value;
    ctx->sawDone = true;
  }
  ctx->lastKey = Key::NONE;
}

void onNull(void* ctxPtr) {
  auto* ctx = static_cast<Ctx*>(ctxPtr);
  // Explicit JSON null for nextCursor means "no next page" -- left
  // indistinguishable from an absent field, since both stop pagination.
  if (ctx->lastKey == Key::NEXT_CURSOR) ctx->out->trailer.hasNextCursor = false;
  ctx->lastKey = Key::NONE;
}

void onObjectStart(void* ctxPtr) {
  auto* ctx = static_cast<Ctx*>(ctxPtr);
  ctx->depth++;
  ctx->lastKey = Key::NONE;
}

void onObjectEnd(void* ctxPtr) {
  auto* ctx = static_cast<Ctx*>(ctxPtr);
  if (ctx->depth > 0) ctx->depth--;
  ctx->lastKey = Key::NONE;
}

void onArrayStart(void* ctxPtr) { static_cast<Ctx*>(ctxPtr)->lastKey = Key::NONE; }
void onArrayEnd(void* ctxPtr) { static_cast<Ctx*>(ctxPtr)->lastKey = Key::NONE; }

}  // namespace

bool parseLine(const char* line, const size_t len, Line& out) {
  out = Line{};
  if (len == 0) return false;  // a blank line (e.g. a trailing newline) is not a record

  Ctx ctx;
  ctx.out = &out;
  StreamingJsonParser parser(JsonCallbacks{&ctx, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd,
                                           onArrayStart, onArrayEnd});
  parser.feed(line, len);
  if (parser.hasError()) return false;

  if (ctx.sawDone) {
    out.kind = Line::Kind::TRAILER;
    return true;
  }
  if (ctx.sawId) {
    out.kind = Line::Kind::ENTRY;
    return true;
  }
  out.kind = Line::Kind::INVALID;
  return false;
}

StreamParser::StreamParser(EntryCallback onEntry, TrailerCallback onTrailer, void* ctx)
    : onEntry_(onEntry), onTrailer_(onTrailer), ctx_(ctx), chunker_(&onLineTrampoline, this, MAX_LINE_LEN) {}

bool StreamParser::feed(const uint8_t* data, const size_t len) { return chunker_.feed(data, len); }

bool StreamParser::onLineTrampoline(void* ctx, const char* line, const size_t len) {
  return static_cast<StreamParser*>(ctx)->onLine(line, len);
}

bool StreamParser::onLine(const char* line, const size_t len) {
  Line parsed;
  if (!parseLine(line, len, parsed)) return false;  // malformed row -- abort the page

  if (parsed.kind == Line::Kind::TRAILER) {
    sawTrailer_ = true;
    trailer_ = parsed.trailer;
    if (onTrailer_) onTrailer_(ctx_, trailer_);
    return true;
  }
  if (onEntry_) return onEntry_(ctx_, parsed.entry);
  return true;
}

}  // namespace wallpaper_manifest
