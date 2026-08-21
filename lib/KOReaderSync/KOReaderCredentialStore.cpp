#include "KOReaderCredentialStore.h"

#include <Logging.h>
#include <Preferences.h>

namespace {
// Bumped when a change to defaults would alter behavior for existing configs.
// v3 dropped the manual username/password/serverUrl/syncBehavior fields: the
// credential now only ever arrives through device pairing, into NVS.
constexpr uint8_t CONFIG_VERSION = 3;

// NVS namespace for the pairing-provisioned credential -- separate from
// SyncCredentialStore's own "cpsync" namespace (different store, different
// lifecycle: this one is cleared on unlink but keyed by KOReaderSync, not
// device pairing). Namespace/keys are well under ESP-IDF's 15-byte cap.
constexpr char PROVISIONED_NVS_NAMESPACE[] = "cpkosync";
constexpr char PROVISIONED_KEY_USERNAME[] = "user";
constexpr char PROVISIONED_KEY_KEYMD5[] = "key";
constexpr char PROVISIONED_KEY_SERVER_URL[] = "url";
}  // namespace

void KOReaderCredentialStore::toJson(JsonDocument& doc) const {
  doc["cfgVersion"] = CONFIG_VERSION;
  doc["matchMethod"] = static_cast<uint8_t>(getMatchMethod());
  doc["sendMetadata"] = getSendMetadata();
}

bool KOReaderCredentialStore::fromJson(JsonVariantConst doc) {
  // Files written before config v3 also carry username/password_obf/serverUrl/
  // syncBehavior. Those fields are gone; they are simply not read, and the
  // resave below rewrites the file without them so the stale obfuscated
  // password does not linger on the SD card.
  const uint8_t cfgVersion = doc["cfgVersion"] | (uint8_t)1;
  bool needsResave = cfgVersion < CONFIG_VERSION;

  uint8_t method = doc["matchMethod"] | (uint8_t)0;
  if (method <= static_cast<uint8_t>(DocumentMatchMethod::BINARY)) {
    setMatchMethod(static_cast<DocumentMatchMethod>(method));
  } else {
    LOG_DBG("KRS", "Invalid matchMethod %u in JSON, resetting to FILENAME", method);
    setMatchMethod(DocumentMatchMethod::FILENAME);
    needsResave = true;
  }
  setSendMetadata(doc["sendMetadata"] | false);

  if (needsResave) {
    LOG_DBG("KRS", "Resaving KOReader settings to update format");
    requestResave();
  }

  return true;
}

void KOReaderCredentialStore::setMatchMethod(DocumentMatchMethod method) {
  matchMethod = method;
  LOG_DBG("KRS", "Set match method: %s", method == DocumentMatchMethod::FILENAME ? "Filename" : "Binary");
}

void KOReaderCredentialStore::setSendMetadata(bool enabled) {
  sendMetadata = enabled;
  LOG_DBG("KRS", "Set send metadata: %s", enabled ? "true" : "false");
}

void KOReaderCredentialStore::setProvisionedCredential(const std::string& username, const std::string& keyMd5,
                                                       const std::string& serverUrl) {
  Preferences prefs;
  if (!prefs.begin(PROVISIONED_NVS_NAMESPACE, /*readOnly=*/false)) {
    LOG_ERR("KRS", "Failed to open NVS namespace to save provisioned credential");
    return;
  }
  prefs.putString(PROVISIONED_KEY_USERNAME, username.c_str());
  prefs.putString(PROVISIONED_KEY_KEYMD5, keyMd5.c_str());
  prefs.putString(PROVISIONED_KEY_SERVER_URL, serverUrl.c_str());
  prefs.end();
  LOG_DBG("KRS", "Provisioned KOSync credential for %s", username.c_str());
}

void KOReaderCredentialStore::clearProvisionedCredential() {
  Preferences prefs;
  if (prefs.begin(PROVISIONED_NVS_NAMESPACE, /*readOnly=*/false)) {
    prefs.clear();
    prefs.end();
  }
  LOG_DBG("KRS", "Cleared provisioned KOSync credential");
}

bool KOReaderCredentialStore::hasProvisionedCredential() const { return !getProvisionedUsername().empty(); }

std::string KOReaderCredentialStore::getProvisionedUsername() const {
  Preferences prefs;
  if (!prefs.begin(PROVISIONED_NVS_NAMESPACE, /*readOnly=*/true)) return "";
  const std::string value = prefs.getString(PROVISIONED_KEY_USERNAME, "").c_str();
  prefs.end();
  return value;
}

std::string KOReaderCredentialStore::getProvisionedKeyMd5() const {
  Preferences prefs;
  if (!prefs.begin(PROVISIONED_NVS_NAMESPACE, /*readOnly=*/true)) return "";
  const std::string value = prefs.getString(PROVISIONED_KEY_KEYMD5, "").c_str();
  prefs.end();
  return value;
}

std::string KOReaderCredentialStore::getProvisionedServerUrl() const {
  Preferences prefs;
  if (!prefs.begin(PROVISIONED_NVS_NAMESPACE, /*readOnly=*/true)) return "";
  const std::string value = prefs.getString(PROVISIONED_KEY_SERVER_URL, "").c_str();
  prefs.end();
  return value;
}
