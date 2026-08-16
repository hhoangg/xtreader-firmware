#pragma once

#include <cstddef>
#include <cstdint>

// Parsers for the RFC 8628 device-authorization JSON bodies exchanged with
// crosspoint-sync's pairing endpoints (see crosspoint-sync docs/API.md,
// "Pairing" section). Deliberately dependency-free (built on the project's
// own StreamingJsonParser, not ArduinoJson): these responses are small --
// well under 1 KB -- so a full parse costs little, but keeping this file
// free of Arduino/ESP-IDF headers is what makes it host-testable under
// test/ without a device.
//
// Fixed-size char buffers, not std::string, matching ReleaseJsonParser
// (lib/JsonParser) -- these values get copied into SyncCredentialStore
// right after parsing, so there is no reason to heap-allocate them here.

// Parsed response of POST /device/code (201).
struct DeviceCodeResponse {
  char deviceCode[128] = {};               // opaque, redeems the token -- never display this
  char userCode[16] = {};                  // e.g. "WDJB-MJHT" -- safe to show/photograph
  char verificationUriComplete[192] = {};  // goes straight into the QR code
  uint32_t expiresIn = 0;                  // seconds; 0 if the field was absent
  uint32_t interval = 0;                   // seconds between polls; 0 if the field was absent
};

// Parsed success response of POST /device/token (200).
struct DeviceTokenResponse {
  char accessToken[200] = {};  // opaque bearer token; caller stores it in NVS, never on SD
  char deviceId[64] = {};
  char deviceName[96] = {};
  char accountEmail[128] = {};
};

// The RFC 8628 error codes POST /device/token answers with (400) before the
// user has approved, or once the code can no longer be redeemed. See
// crosspoint-sync docs/API.md's table -- EXPIRED_TOKEN deliberately covers
// both "actually expired" and "unknown code" so a caller guessing device
// codes learns nothing from the response.
enum class DeviceTokenPollError : uint8_t {
  NONE = 0,  // body didn't parse, or "error" was missing/not one of the four below
  AUTHORIZATION_PENDING,
  SLOW_DOWN,
  ACCESS_DENIED,
  EXPIRED_TOKEN,
};

// Parses the 201 body of POST /device/code. Returns false if the JSON is
// malformed or missing any of deviceCode/userCode/verificationUriComplete.
// expiresIn/interval are 0 when absent; the caller should fall back to the
// documented defaults (300s / 15s) rather than treat 0 as "expired now".
bool parseDeviceCodeResponse(const char* json, size_t len, DeviceCodeResponse& out);

// Parses the 200 body of POST /device/token. Returns false if the JSON is
// malformed or missing accessToken.
bool parseDeviceTokenSuccess(const char* json, size_t len, DeviceTokenResponse& out);

// Parses the 400 error body of POST /device/token, e.g.
// {"error":"authorization_pending"}. Returns NONE if the body doesn't parse
// or "error" isn't one of the four RFC 8628 codes this API defines.
DeviceTokenPollError parseDeviceTokenPollError(const char* json, size_t len);
