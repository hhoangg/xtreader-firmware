#include "DevicePairingProtocol.h"

#include <cstring>

#include "StreamingJsonParser.h"

namespace {

void safeCopy(char* dst, size_t dstSize, const char* src, size_t srcLen) {
  const size_t n = srcLen < dstSize - 1 ? srcLen : dstSize - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

// Compares a SAX token (not NUL-terminated) against a NUL-terminated literal.
bool matches(const char* value, size_t len, const char* literal) {
  return len == strlen(literal) && memcmp(value, literal, len) == 0;
}

uint32_t parseUintToken(const char* value, size_t len) {
  uint32_t result = 0;
  for (size_t i = 0; i < len; i++) {
    const char c = value[i];
    if (c < '0' || c > '9') break;
    result = result * 10 + static_cast<uint32_t>(c - '0');
  }
  return result;
}

// --- POST /device/code ------------------------------------------------------
// Flat object: {"deviceCode","userCode","verificationUri","verificationUriComplete","expiresIn","interval"}.

enum class CodeKey : uint8_t { NONE, DEVICE_CODE, USER_CODE, VERIFICATION_URI_COMPLETE, EXPIRES_IN, INTERVAL };

struct CodeCtx {
  DeviceCodeResponse* out;
  CodeKey lastKey = CodeKey::NONE;
  uint8_t depth = 0;
  bool haveDeviceCode = false;
  bool haveUserCode = false;
  bool haveVerificationUriComplete = false;
};

void codeOnKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<CodeCtx*>(ctx);
  self->lastKey = CodeKey::NONE;
  if (self->depth != 1) return;
  if (matches(key, len, "deviceCode"))
    self->lastKey = CodeKey::DEVICE_CODE;
  else if (matches(key, len, "userCode"))
    self->lastKey = CodeKey::USER_CODE;
  else if (matches(key, len, "verificationUriComplete"))
    self->lastKey = CodeKey::VERIFICATION_URI_COMPLETE;
  else if (matches(key, len, "expiresIn"))
    self->lastKey = CodeKey::EXPIRES_IN;
  else if (matches(key, len, "interval"))
    self->lastKey = CodeKey::INTERVAL;
}

void codeOnString(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<CodeCtx*>(ctx);
  switch (self->lastKey) {
    case CodeKey::DEVICE_CODE:
      safeCopy(self->out->deviceCode, sizeof(self->out->deviceCode), value, len);
      self->haveDeviceCode = true;
      break;
    case CodeKey::USER_CODE:
      safeCopy(self->out->userCode, sizeof(self->out->userCode), value, len);
      self->haveUserCode = true;
      break;
    case CodeKey::VERIFICATION_URI_COMPLETE:
      safeCopy(self->out->verificationUriComplete, sizeof(self->out->verificationUriComplete), value, len);
      self->haveVerificationUriComplete = true;
      break;
    default:
      break;
  }
  self->lastKey = CodeKey::NONE;
}

void codeOnNumber(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<CodeCtx*>(ctx);
  if (self->lastKey == CodeKey::EXPIRES_IN)
    self->out->expiresIn = parseUintToken(value, len);
  else if (self->lastKey == CodeKey::INTERVAL)
    self->out->interval = parseUintToken(value, len);
  self->lastKey = CodeKey::NONE;
}

void codeOnBool(void* ctx, bool) { static_cast<CodeCtx*>(ctx)->lastKey = CodeKey::NONE; }
void codeOnNull(void* ctx) { static_cast<CodeCtx*>(ctx)->lastKey = CodeKey::NONE; }

void codeOnObjectStart(void* ctx) {
  auto* self = static_cast<CodeCtx*>(ctx);
  self->depth++;
  self->lastKey = CodeKey::NONE;
}

void codeOnObjectEnd(void* ctx) {
  auto* self = static_cast<CodeCtx*>(ctx);
  if (self->depth > 0) self->depth--;
  self->lastKey = CodeKey::NONE;
}

void codeOnArrayStart(void* ctx) { static_cast<CodeCtx*>(ctx)->lastKey = CodeKey::NONE; }
void codeOnArrayEnd(void* ctx) { static_cast<CodeCtx*>(ctx)->lastKey = CodeKey::NONE; }

// --- POST /device/token success ---------------------------------------------
// {"accessToken","deviceId","deviceName","account":{"email",...},"kosync":{"key",...}}.

enum class TokenKey : uint8_t { NONE, ACCESS_TOKEN, DEVICE_ID, DEVICE_NAME, ACCOUNT, EMAIL, KOSYNC, KOSYNC_KEY };

struct TokenCtx {
  DeviceTokenResponse* out;
  TokenKey lastKey = TokenKey::NONE;
  uint8_t depth = 0;
  bool inAccount = false;
  bool inKosync = false;
  bool haveAccessToken = false;
};

void tokenOnKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<TokenCtx*>(ctx);
  self->lastKey = TokenKey::NONE;
  if (self->inAccount && self->depth == 2) {
    if (matches(key, len, "email")) self->lastKey = TokenKey::EMAIL;
    return;
  }
  if (self->inKosync && self->depth == 2) {
    if (matches(key, len, "key")) self->lastKey = TokenKey::KOSYNC_KEY;
    return;
  }
  if (self->depth != 1) return;
  if (matches(key, len, "accessToken"))
    self->lastKey = TokenKey::ACCESS_TOKEN;
  else if (matches(key, len, "deviceId"))
    self->lastKey = TokenKey::DEVICE_ID;
  else if (matches(key, len, "deviceName"))
    self->lastKey = TokenKey::DEVICE_NAME;
  else if (matches(key, len, "account"))
    self->lastKey = TokenKey::ACCOUNT;
  else if (matches(key, len, "kosync"))
    self->lastKey = TokenKey::KOSYNC;
}

void tokenOnString(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<TokenCtx*>(ctx);
  switch (self->lastKey) {
    case TokenKey::ACCESS_TOKEN:
      safeCopy(self->out->accessToken, sizeof(self->out->accessToken), value, len);
      self->haveAccessToken = true;
      break;
    case TokenKey::DEVICE_ID:
      safeCopy(self->out->deviceId, sizeof(self->out->deviceId), value, len);
      break;
    case TokenKey::DEVICE_NAME:
      safeCopy(self->out->deviceName, sizeof(self->out->deviceName), value, len);
      break;
    case TokenKey::EMAIL:
      safeCopy(self->out->accountEmail, sizeof(self->out->accountEmail), value, len);
      break;
    case TokenKey::KOSYNC_KEY:
      safeCopy(self->out->kosyncKey, sizeof(self->out->kosyncKey), value, len);
      break;
    default:
      break;
  }
  self->lastKey = TokenKey::NONE;
}

void tokenOnNumber(void* ctx, const char*, size_t) { static_cast<TokenCtx*>(ctx)->lastKey = TokenKey::NONE; }
void tokenOnBool(void* ctx, bool) { static_cast<TokenCtx*>(ctx)->lastKey = TokenKey::NONE; }
void tokenOnNull(void* ctx) { static_cast<TokenCtx*>(ctx)->lastKey = TokenKey::NONE; }

void tokenOnObjectStart(void* ctx) {
  auto* self = static_cast<TokenCtx*>(ctx);
  if (self->lastKey == TokenKey::ACCOUNT && self->depth == 1) self->inAccount = true;
  if (self->lastKey == TokenKey::KOSYNC && self->depth == 1) self->inKosync = true;
  self->depth++;
  self->lastKey = TokenKey::NONE;
}

void tokenOnObjectEnd(void* ctx) {
  auto* self = static_cast<TokenCtx*>(ctx);
  if (self->depth > 0) self->depth--;
  if (self->depth <= 1) {
    self->inAccount = false;
    self->inKosync = false;
  }
  self->lastKey = TokenKey::NONE;
}

void tokenOnArrayStart(void* ctx) { static_cast<TokenCtx*>(ctx)->lastKey = TokenKey::NONE; }
void tokenOnArrayEnd(void* ctx) { static_cast<TokenCtx*>(ctx)->lastKey = TokenKey::NONE; }

// --- POST /device/token error ------------------------------------------------
// {"error":"authorization_pending" | "slow_down" | "access_denied" | "expired_token"}.

struct ErrCtx {
  bool matchingError = false;
  uint8_t depth = 0;
  DeviceTokenPollError result = DeviceTokenPollError::NONE;
};

void errOnKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<ErrCtx*>(ctx);
  self->matchingError = self->depth == 1 && matches(key, len, "error");
}

void errOnString(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<ErrCtx*>(ctx);
  if (self->matchingError) {
    if (matches(value, len, "authorization_pending"))
      self->result = DeviceTokenPollError::AUTHORIZATION_PENDING;
    else if (matches(value, len, "slow_down"))
      self->result = DeviceTokenPollError::SLOW_DOWN;
    else if (matches(value, len, "access_denied"))
      self->result = DeviceTokenPollError::ACCESS_DENIED;
    else if (matches(value, len, "expired_token"))
      self->result = DeviceTokenPollError::EXPIRED_TOKEN;
  }
  self->matchingError = false;
}

void errOnNumber(void* ctx, const char*, size_t) { static_cast<ErrCtx*>(ctx)->matchingError = false; }
void errOnBool(void* ctx, bool) { static_cast<ErrCtx*>(ctx)->matchingError = false; }
void errOnNull(void* ctx) { static_cast<ErrCtx*>(ctx)->matchingError = false; }

void errOnObjectStart(void* ctx) {
  auto* self = static_cast<ErrCtx*>(ctx);
  self->depth++;
  self->matchingError = false;
}

void errOnObjectEnd(void* ctx) {
  auto* self = static_cast<ErrCtx*>(ctx);
  if (self->depth > 0) self->depth--;
  self->matchingError = false;
}

void errOnArrayStart(void* ctx) { static_cast<ErrCtx*>(ctx)->matchingError = false; }
void errOnArrayEnd(void* ctx) { static_cast<ErrCtx*>(ctx)->matchingError = false; }

}  // namespace

bool parseDeviceCodeResponse(const char* json, size_t len, DeviceCodeResponse& out) {
  out = DeviceCodeResponse{};
  CodeCtx ctx;
  ctx.out = &out;
  StreamingJsonParser parser(JsonCallbacks{&ctx, codeOnKey, codeOnString, codeOnNumber, codeOnBool, codeOnNull,
                                           codeOnObjectStart, codeOnObjectEnd, codeOnArrayStart, codeOnArrayEnd});
  parser.feed(json, len);
  if (parser.hasError()) return false;
  return ctx.haveDeviceCode && ctx.haveUserCode && ctx.haveVerificationUriComplete;
}

bool parseDeviceTokenSuccess(const char* json, size_t len, DeviceTokenResponse& out) {
  out = DeviceTokenResponse{};
  TokenCtx ctx;
  ctx.out = &out;
  StreamingJsonParser parser(JsonCallbacks{&ctx, tokenOnKey, tokenOnString, tokenOnNumber, tokenOnBool, tokenOnNull,
                                           tokenOnObjectStart, tokenOnObjectEnd, tokenOnArrayStart, tokenOnArrayEnd});
  parser.feed(json, len);
  if (parser.hasError()) return false;
  return ctx.haveAccessToken;
}

DeviceTokenPollError parseDeviceTokenPollError(const char* json, size_t len) {
  ErrCtx ctx;
  StreamingJsonParser parser(JsonCallbacks{&ctx, errOnKey, errOnString, errOnNumber, errOnBool, errOnNull,
                                           errOnObjectStart, errOnObjectEnd, errOnArrayStart, errOnArrayEnd});
  parser.feed(json, len);
  return ctx.result;
}
