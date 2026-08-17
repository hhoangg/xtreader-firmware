#include <gtest/gtest.h>

#include <cstring>

#include "DevicePairingProtocol.h"

namespace {
bool parseCode(const char* json, DeviceCodeResponse& out) { return parseDeviceCodeResponse(json, strlen(json), out); }
bool parseTokenOk(const char* json, DeviceTokenResponse& out) {
  return parseDeviceTokenSuccess(json, strlen(json), out);
}
DeviceTokenPollError parseErr(const char* json) { return parseDeviceTokenPollError(json, strlen(json)); }
}  // namespace

// --- POST /device/code -------------------------------------------------

TEST(DeviceCodeResponse, ParsesRealisticBody) {
  const char* json = R"({
    "deviceCode": "opaque-secret-value-do-not-show",
    "userCode": "WDJB-MJHT",
    "verificationUri": "https://crosspoint-sync.example/link",
    "verificationUriComplete": "https://crosspoint-sync.example/link?code=WDJB-MJHT",
    "expiresIn": 300,
    "interval": 15
  })";
  DeviceCodeResponse out;
  ASSERT_TRUE(parseCode(json, out));
  EXPECT_STREQ(out.deviceCode, "opaque-secret-value-do-not-show");
  EXPECT_STREQ(out.userCode, "WDJB-MJHT");
  EXPECT_STREQ(out.verificationUriComplete, "https://crosspoint-sync.example/link?code=WDJB-MJHT");
  EXPECT_EQ(out.expiresIn, 300u);
  EXPECT_EQ(out.interval, 15u);
}

TEST(DeviceCodeResponse, MissingRequiredFieldFails) {
  const char* json = R"({"deviceCode": "abc", "expiresIn": 300, "interval": 15})";
  DeviceCodeResponse out;
  // userCode and verificationUriComplete are both missing.
  EXPECT_FALSE(parseCode(json, out));
}

TEST(DeviceCodeResponse, MissingOptionalNumbersDefaultToZero) {
  const char* json = R"({"deviceCode": "abc", "userCode": "WDJB-MJHT", "verificationUriComplete": "https://x/y"})";
  DeviceCodeResponse out;
  ASSERT_TRUE(parseCode(json, out));
  EXPECT_EQ(out.expiresIn, 0u);
  EXPECT_EQ(out.interval, 0u);
}

TEST(DeviceCodeResponse, MalformedJsonFails) {
  DeviceCodeResponse out;
  EXPECT_FALSE(parseCode("{not json", out));
}

TEST(DeviceCodeResponse, OverlongFieldIsTruncatedNotOverflowed) {
  std::string longCode(500, 'x');
  std::string json = std::string(R"({"deviceCode": ")") + longCode +
                     R"(", "userCode": "WDJB-MJHT", "verificationUriComplete": "https://x/y"})";
  DeviceCodeResponse out;
  ASSERT_TRUE(parseCode(json.c_str(), out));
  EXPECT_EQ(strlen(out.deviceCode), sizeof(out.deviceCode) - 1);
}

// --- POST /device/token success -----------------------------------------

TEST(DeviceTokenResponse, ParsesRealisticBody) {
  const char* json = R"({
    "accessToken": "tok_abcdef123456",
    "deviceId": "dev_xyz",
    "deviceName": "X4 của Lan",
    "account": { "id": "usr_1", "email": "lan@example.com", "displayName": "Lan" },
    "kosync": { "username": "lan@example.com", "key": "0123456789abcdef0123456789abcdef" }
  })";
  DeviceTokenResponse out;
  ASSERT_TRUE(parseTokenOk(json, out));
  EXPECT_STREQ(out.accessToken, "tok_abcdef123456");
  EXPECT_STREQ(out.deviceId, "dev_xyz");
  EXPECT_STREQ(out.accountEmail, "lan@example.com");
  EXPECT_STREQ(out.kosyncKey, "0123456789abcdef0123456789abcdef");
}

TEST(DeviceTokenResponse, MissingAccessTokenFails) {
  const char* json = R"({"deviceId": "dev_xyz", "account": {"email": "a@b.com"}})";
  DeviceTokenResponse out;
  EXPECT_FALSE(parseTokenOk(json, out));
}

TEST(DeviceTokenResponse, MissingAccountObjectStillSucceeds) {
  // A stock KOSync-style response without the CrossPoint "account" extension
  // should not fail the whole parse -- just leave accountEmail empty.
  const char* json = R"({"accessToken": "tok_1", "deviceId": "dev_1", "deviceName": "X4"})";
  DeviceTokenResponse out;
  ASSERT_TRUE(parseTokenOk(json, out));
  EXPECT_STREQ(out.accessToken, "tok_1");
  EXPECT_STREQ(out.accountEmail, "");
}

TEST(DeviceTokenResponse, MissingKosyncObjectStillSucceeds) {
  // A server that predates KOSync provisioning omits "kosync" entirely --
  // pairing must still succeed, just without a provisioned credential.
  const char* json = R"({"accessToken": "tok_1", "deviceId": "dev_1", "account": {"email": "a@b.com"}})";
  DeviceTokenResponse out;
  ASSERT_TRUE(parseTokenOk(json, out));
  EXPECT_STREQ(out.accountEmail, "a@b.com");
  EXPECT_STREQ(out.kosyncKey, "");
}

TEST(DeviceTokenResponse, DoesNotConfuseAccountAndKosyncFields) {
  // Both nested objects use different keys ("email" vs "key"), but this
  // guards the depth/flag tracking that keeps them from bleeding into each
  // other regardless of which one appears first.
  const char* json = R"({
    "accessToken": "tok_1",
    "kosync": { "username": "a@b.com", "key": "11112222333344445555666677778888" },
    "account": { "id": "usr_1", "email": "a@b.com", "displayName": "A" }
  })";
  DeviceTokenResponse out;
  ASSERT_TRUE(parseTokenOk(json, out));
  EXPECT_STREQ(out.accountEmail, "a@b.com");
  EXPECT_STREQ(out.kosyncKey, "11112222333344445555666677778888");
}

// --- POST /device/token error --------------------------------------------

TEST(DeviceTokenPollError, ParsesAllFourRfc8628Codes) {
  EXPECT_EQ(parseErr(R"({"error":"authorization_pending"})"), DeviceTokenPollError::AUTHORIZATION_PENDING);
  EXPECT_EQ(parseErr(R"({"error":"slow_down"})"), DeviceTokenPollError::SLOW_DOWN);
  EXPECT_EQ(parseErr(R"({"error":"access_denied"})"), DeviceTokenPollError::ACCESS_DENIED);
  EXPECT_EQ(parseErr(R"({"error":"expired_token"})"), DeviceTokenPollError::EXPIRED_TOKEN);
}

TEST(DeviceTokenPollError, UnknownErrorStringIsNone) {
  EXPECT_EQ(parseErr(R"({"error":"something_else"})"), DeviceTokenPollError::NONE);
}

TEST(DeviceTokenPollError, MissingErrorFieldIsNone) {
  EXPECT_EQ(parseErr(R"({"message":"nope"})"), DeviceTokenPollError::NONE);
}

TEST(DeviceTokenPollError, MalformedJsonIsNone) { EXPECT_EQ(parseErr("not json at all"), DeviceTokenPollError::NONE); }
