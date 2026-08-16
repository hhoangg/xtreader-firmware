#include "ManifestLineParser.h"

#include <cstring>

#include "StreamingJsonParser.h"

namespace {

bool matches(const char* value, size_t len, const char* literal) {
  return len == strlen(literal) && memcmp(value, literal, len) == 0;
}

uint64_t parseUint64Token(const char* value, size_t len) {
  uint64_t result = 0;
  for (size_t i = 0; i < len; i++) {
    const char c = value[i];
    if (c < '0' || c > '9') break;
    result = result * 10 + static_cast<uint64_t>(c - '0');
  }
  return result;
}

// Every field either an entry row or the trailer can carry, at the line's
// top-level object (depth 1). One flat enum for both shapes -- simpler than
// two parses, since a line is only ever one or the other and the fields
// don't collide by name.
enum class Key : uint8_t {
  NONE,
  ID,
  PATH,
  SIZE_BYTES,
  CONTENT_HASH,
  UPDATED_AT,
  DELETED,
  DONE,
  NEXT_CURSOR,
  TOTAL_COUNT,
};

struct Ctx {
  ManifestLine* out;
  Key lastKey = Key::NONE;
  uint8_t depth = 0;
  bool sawId = false;
  bool sawPath = false;
  bool sawDone = false;
};

void onKey(void* ctxPtr, const char* key, size_t len) {
  auto* ctx = static_cast<Ctx*>(ctxPtr);
  ctx->lastKey = Key::NONE;
  if (ctx->depth != 1) return;
  if (matches(key, len, "id"))
    ctx->lastKey = Key::ID;
  else if (matches(key, len, "path"))
    ctx->lastKey = Key::PATH;
  else if (matches(key, len, "sizeBytes"))
    ctx->lastKey = Key::SIZE_BYTES;
  else if (matches(key, len, "contentHash"))
    ctx->lastKey = Key::CONTENT_HASH;
  else if (matches(key, len, "updatedAt"))
    ctx->lastKey = Key::UPDATED_AT;
  else if (matches(key, len, "deleted"))
    ctx->lastKey = Key::DELETED;
  else if (matches(key, len, "done"))
    ctx->lastKey = Key::DONE;
  else if (matches(key, len, "nextCursor"))
    ctx->lastKey = Key::NEXT_CURSOR;
  else if (matches(key, len, "totalCount"))
    ctx->lastKey = Key::TOTAL_COUNT;
}

void onString(void* ctxPtr, const char* value, size_t len) {
  auto* ctx = static_cast<Ctx*>(ctxPtr);
  switch (ctx->lastKey) {
    case Key::ID:
      ctx->out->entry.id.assign(value, len);
      ctx->sawId = true;
      break;
    case Key::PATH:
      ctx->out->entry.path.assign(value, len);
      ctx->sawPath = true;
      break;
    case Key::CONTENT_HASH:
      ctx->out->entry.contentHash.assign(value, len);
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

void onNumber(void* ctxPtr, const char* value, size_t len) {
  auto* ctx = static_cast<Ctx*>(ctxPtr);
  switch (ctx->lastKey) {
    case Key::SIZE_BYTES:
      ctx->out->entry.sizeBytes = parseUint64Token(value, len);
      break;
    case Key::UPDATED_AT:
      ctx->out->entry.updatedAt = parseUint64Token(value, len);
      break;
    case Key::TOTAL_COUNT:
      ctx->out->trailer.totalCount = static_cast<uint32_t>(parseUint64Token(value, len));
      break;
    default:
      break;
  }
  ctx->lastKey = Key::NONE;
}

void onBool(void* ctxPtr, bool value) {
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
  if (ctx->lastKey == Key::NEXT_CURSOR) {
    // Explicit JSON null: "no next page". Leave hasNextCursor false (its
    // default) so the caller can tell "field present but null" apart from
    // "field never sent" the same way -- both mean stop paginating.
    ctx->out->trailer.hasNextCursor = false;
  }
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

bool parseManifestLine(const char* line, size_t len, ManifestLine& out) {
  out = ManifestLine{};
  if (len == 0) return false;  // a blank line (e.g. trailing newline) is not a record

  Ctx ctx;
  ctx.out = &out;
  StreamingJsonParser parser(
      JsonCallbacks{&ctx, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd, onArrayStart, onArrayEnd});
  parser.feed(line, len);
  if (parser.hasError()) return false;

  if (ctx.sawDone) {
    out.kind = ManifestLine::Kind::TRAILER;
    return true;
  }
  if (ctx.sawId && ctx.sawPath) {
    out.kind = ManifestLine::Kind::ENTRY;
    return true;
  }
  out.kind = ManifestLine::Kind::INVALID;
  return false;
}
