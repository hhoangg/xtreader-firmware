#include "SyncCredentialStore.h"

#include <Logging.h>
#include <Preferences.h>

namespace {
// NVS namespace names are capped at 15 bytes on ESP-IDF; "cpsync" is well
// under that. Keys likewise (max 15 bytes) -- all comfortably short here.
constexpr char NVS_NAMESPACE[] = "cpsync";
constexpr char KEY_SERVER_URL[] = "url";
constexpr char KEY_ACCESS_TOKEN[] = "tok";
constexpr char KEY_DEVICE_ID[] = "devId";
constexpr char KEY_DEVICE_NAME[] = "devName";
constexpr char KEY_ACCOUNT_EMAIL[] = "email";

// Default CrossPoint Sync server; self-hosters point elsewhere via the
// "Server URL" setting (see SettingsList.h / SyncSettingsActivity).
//
// The custom domain rather than the workers.dev hostname it also answers on.
// This string is what a fresh device sends /device/code to, and the server
// mirrors that request's own origin straight back as verificationUriComplete
// -- which is the string the pairing QR encodes. Point this at workers.dev
// and every reader shows a QR for a hostname its owner never bought.
//
// Safe to change: a reader that is already paired keeps the URL it paired
// against in NVS (see get/setServerUrl below), and the workers.dev hostname
// stays live alongside this one, so only new pairings move.
constexpr char DEFAULT_SERVER_URL[] = "https://xtreader.com";
}  // namespace

SyncCredentialStore& SyncCredentialStore::getInstance() {
  static SyncCredentialStore instance;
  return instance;
}

bool SyncCredentialStore::load() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/true)) {
    // No namespace yet (first boot, nothing ever written) -- not an error,
    // just nothing paired and no URL override.
    serverUrl_.clear();
    accessToken_.clear();
    deviceId_.clear();
    deviceName_.clear();
    accountEmail_.clear();
    return true;
  }
  serverUrl_ = prefs.getString(KEY_SERVER_URL, "").c_str();
  accessToken_ = prefs.getString(KEY_ACCESS_TOKEN, "").c_str();
  deviceId_ = prefs.getString(KEY_DEVICE_ID, "").c_str();
  deviceName_ = prefs.getString(KEY_DEVICE_NAME, "").c_str();
  accountEmail_ = prefs.getString(KEY_ACCOUNT_EMAIL, "").c_str();
  prefs.end();
  LOG_DBG("SYNC", "Loaded sync credentials from NVS (paired=%s)", isPaired() ? "yes" : "no");
  return true;
}

void SyncCredentialStore::setServerUrl(const std::string& url) {
  serverUrl_ = url;
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
    LOG_ERR("SYNC", "Failed to open NVS namespace to save server URL");
    return;
  }
  prefs.putString(KEY_SERVER_URL, serverUrl_.c_str());
  prefs.end();
}

std::string SyncCredentialStore::getBaseUrl() const {
  std::string url = serverUrl_.empty() ? DEFAULT_SERVER_URL : serverUrl_;
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  return url;
}

bool SyncCredentialStore::setPairing(const std::string& accessToken, const std::string& deviceId,
                                     const std::string& deviceName, const std::string& accountEmail) {
  if (accessToken.empty()) {
    LOG_ERR("SYNC", "Refusing to persist an empty access token");
    return false;
  }
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
    LOG_ERR("SYNC", "Failed to open NVS namespace to save pairing");
    return false;
  }
  prefs.putString(KEY_ACCESS_TOKEN, accessToken.c_str());
  prefs.putString(KEY_DEVICE_ID, deviceId.c_str());
  prefs.putString(KEY_DEVICE_NAME, deviceName.c_str());
  prefs.putString(KEY_ACCOUNT_EMAIL, accountEmail.c_str());
  prefs.end();

  accessToken_ = accessToken;
  deviceId_ = deviceId;
  deviceName_ = deviceName;
  accountEmail_ = accountEmail;
  LOG_DBG("SYNC", "Paired as %s (device %s)", accountEmail_.c_str(), deviceId_.c_str());
  return true;
}

void SyncCredentialStore::clearPairing() {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
    prefs.remove(KEY_ACCESS_TOKEN);
    prefs.remove(KEY_DEVICE_ID);
    prefs.remove(KEY_DEVICE_NAME);
    prefs.remove(KEY_ACCOUNT_EMAIL);
    prefs.end();
  }
  accessToken_.clear();
  deviceId_.clear();
  deviceName_.clear();
  accountEmail_.clear();
  LOG_DBG("SYNC", "Cleared sync pairing");
}

void SyncCredentialStore::clearAll() {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
    prefs.clear();
    prefs.end();
  }
  serverUrl_.clear();
  accessToken_.clear();
  deviceId_.clear();
  deviceName_.clear();
  accountEmail_.clear();
  LOG_DBG("SYNC", "Cleared all sync credentials (server URL reset to default)");
}
