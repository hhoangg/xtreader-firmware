#include "KOReaderCredentialStore.h"

#include <Logging.h>
#include <MD5Builder.h>
#include <ObfuscationUtils.h>
#include <Preferences.h>

namespace {
// Default sync server URL. crosspoint-sync speaks the full KOSync protocol, so
// pointing at any other kosync server (e.g. https://sync.koreader.rocks:443)
// still works via the custom server URL setting.
constexpr char DEFAULT_SERVER_URL[] = "https://sync.crosspointreader.com";

// Default before config version 2. Configs saved without a version stamp and an
// empty serverUrl were implicitly syncing here — they get pinned on upgrade.
constexpr char LEGACY_DEFAULT_SERVER_URL[] = "https://sync.koreader.rocks:443";

// Bumped when a change to defaults would alter behavior for existing configs.
constexpr uint8_t CONFIG_VERSION = 2;

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
  doc["username"] = getUsername();
  doc["password_obf"] = obfuscation::obfuscateToBase64(getPassword());
  doc["serverUrl"] = getServerUrl();
  doc["matchMethod"] = static_cast<uint8_t>(getMatchMethod());
  doc["sendMetadata"] = getSendMetadata();
  doc["syncBehavior"] = static_cast<uint8_t>(getSyncBehavior());
}

bool KOReaderCredentialStore::fromJson(JsonVariantConst doc) {
  std::string user = doc["username"] | "";

  bool needsResave = false;
  std::string pass = extractPassword(doc, needsResave);

  setCredentials(user, pass);
  setServerUrl(doc["serverUrl"] | "");

  // The default server changed in config v2 (sync.koreader.rocks -> crosspoint-sync).
  // A pre-v2 config with credentials and no explicit URL was actively syncing
  // against the old default — pin that URL so the upgrade doesn't switch servers
  // out from under the user. Fresh setups get the new default.
  const uint8_t cfgVersion = doc["cfgVersion"] | (uint8_t)1;
  if (cfgVersion < CONFIG_VERSION) {
    if (getServerUrl().empty() && hasCredentials()) {
      LOG_DBG("KRS", "Pre-v2 config used the old default server; pinning %s", LEGACY_DEFAULT_SERVER_URL);
      setServerUrl(LEGACY_DEFAULT_SERVER_URL);
    }
    needsResave = true;  // stamp cfgVersion so this migration runs once
  }

  uint8_t method = doc["matchMethod"] | (uint8_t)0;
  if (method <= static_cast<uint8_t>(DocumentMatchMethod::BINARY)) {
    setMatchMethod(static_cast<DocumentMatchMethod>(method));
  } else {
    LOG_DBG("KRS", "Invalid matchMethod %u in JSON, resetting to FILENAME", method);
    setMatchMethod(DocumentMatchMethod::FILENAME);
  }
  setSendMetadata(doc["sendMetadata"] | false);

  const JsonVariantConst behaviorValue = doc["syncBehavior"];
  const bool missingBehavior = behaviorValue.isNull();
  uint8_t behavior = behaviorValue | static_cast<uint8_t>(KOReaderSyncBehavior::ASK_EVERY_TIME);
  if (behavior <= static_cast<uint8_t>(KOReaderSyncBehavior::SMART)) {
    setSyncBehavior(static_cast<KOReaderSyncBehavior>(behavior));
    needsResave = needsResave || missingBehavior;
  } else {
    LOG_DBG("KRS", "Invalid syncBehavior %u in JSON, resetting to ASK_EVERY_TIME", behavior);
    setSyncBehavior(KOReaderSyncBehavior::ASK_EVERY_TIME);
    needsResave = true;
  }

  if (needsResave) {
    LOG_DBG("KRS", "Resaving KOReader credentials to update format");
    requestResave();
  }

  return true;
}

void KOReaderCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  username = user;
  password = pass;
  LOG_DBG("KRS", "Set credentials for user: %s", user.c_str());
}

std::string KOReaderCredentialStore::getMd5Password() const {
  if (password.empty()) {
    return "";
  }

  // Calculate MD5 hash of password using ESP32's MD5Builder
  MD5Builder md5;
  md5.begin();
  md5.add(password.c_str());
  md5.calculate();

  return md5.toString().c_str();
}

bool KOReaderCredentialStore::hasCredentials() const { return !username.empty() && !password.empty(); }

void KOReaderCredentialStore::clearCredentials() {
  username.clear();
  password.clear();
  saveToFile();
  LOG_DBG("KRS", "Cleared KOReader credentials");
}

void KOReaderCredentialStore::setServerUrl(const std::string& url) {
  serverUrl = url;
  LOG_DBG("KRS", "Set server URL: %s", url.empty() ? "(default)" : url.c_str());
}

std::string KOReaderCredentialStore::getBaseUrl() const {
  std::string url;
  if (serverUrl.empty()) {
    url = DEFAULT_SERVER_URL;
  } else if (serverUrl.find("://") == std::string::npos) {
    // Normalize URL: add http:// if no protocol specified (local servers typically don't have SSL)
    url = "http://" + serverUrl;
  } else {
    url = serverUrl;
  }

  // Strip trailing slashes to avoid double-slash in API paths
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }

  return url;
}

bool KOReaderCredentialStore::usesCrossPointSyncServer() const { return getBaseUrl() == DEFAULT_SERVER_URL; }

void KOReaderCredentialStore::setMatchMethod(DocumentMatchMethod method) {
  matchMethod = method;
  LOG_DBG("KRS", "Set match method: %s", method == DocumentMatchMethod::FILENAME ? "Filename" : "Binary");
}

void KOReaderCredentialStore::setSendMetadata(bool enabled) {
  sendMetadata = enabled;
  LOG_DBG("KRS", "Set send metadata: %s", enabled ? "true" : "false");
}

void KOReaderCredentialStore::setSyncBehavior(KOReaderSyncBehavior behavior) {
  if (static_cast<uint8_t>(behavior) > static_cast<uint8_t>(KOReaderSyncBehavior::SMART)) {
    behavior = KOReaderSyncBehavior::ASK_EVERY_TIME;
  }
  syncBehavior = behavior;
  LOG_DBG("KRS", "Set sync behavior: %s", behavior == KOReaderSyncBehavior::SMART ? "Smart" : "Ask");
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
