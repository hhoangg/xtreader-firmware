#pragma once

#include <string>

/**
 * Singleton store for CrossPoint Sync device-pairing state: the self-hoster's
 * server base URL override, the (never-expiring) bearer token issued by
 * POST /device/token, and the account/device identifiers that came with it.
 *
 * Deliberately persisted to NVS via Preferences.h, NOT to the SD card the
 * way WifiCredentialStore/KOReaderCredentialStore are. Those stores write
 * (obfuscated, not encrypted) JSON to the SD card, which anyone with a card
 * reader can pull and read offline; a bearer token that never expires --
 * full read/write access to the account's library and reading progress
 * until the owner deletes the device server-side -- must not live there.
 * NVS lives in the internal flash chip behind the same access path as the
 * firmware image itself, not on a removable card. It also doesn't share the
 * SD card's SPI bus, so unlike WifiCredentialStore this store needs no
 * RenderLock around load()/save -- see WifiSelectionActivity::onEnter() for
 * the SD-card equivalent this sidesteps.
 *
 * partitions.csv reserves a 20 KB "nvs" partition; this store's five string
 * fields use well under 1 KB of it even at their max declared length.
 */
class SyncCredentialStore {
 public:
  static SyncCredentialStore& getInstance();

  SyncCredentialStore(const SyncCredentialStore&) = delete;
  SyncCredentialStore& operator=(const SyncCredentialStore&) = delete;

  // Reads persisted state from NVS into memory. Safe to call more than once
  // (e.g. after a self-hoster edits the server URL from the web UI on
  // another device); call once at boot before using the getters below.
  bool load();

  // --- Server URL (self-hoster override; empty = use the default) --------
  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const { return serverUrl_; }
  // Normalized for use in requests: default substituted for an empty
  // override, trailing slashes stripped.
  std::string getBaseUrl() const;

  // --- Pairing result (POST /device/token's 200 response) ----------------
  // Persists all four fields together so a partial write can never leave a
  // token on disk with a mismatched device id/name.
  bool setPairing(const std::string& accessToken, const std::string& deviceId, const std::string& deviceName,
                  const std::string& accountEmail);
  bool isPaired() const { return !accessToken_.empty(); }
  const std::string& getAccessToken() const { return accessToken_; }
  const std::string& getDeviceId() const { return deviceId_; }
  const std::string& getDeviceName() const { return deviceName_; }
  const std::string& getAccountEmail() const { return accountEmail_; }

  // --- First-sync seed marker -------------------------------------------
  // True once a manifest sync has completed against this pairing. It is what
  // makes recent_discovery::decide()'s firstSync exactly one sync long: the
  // first sync after pairing must insert nothing, or a freshly paired device
  // pulls the owner's entire library into the recency list as "new" (see
  // docs/superpowers/specs/2026-09-02-one-recency-list-design.md). Kept with
  // the pairing rather than in state.json so clearPairing() below clears it
  // for free -- one place forgets an account, not two -- and a re-pair
  // behaves like a fresh device.
  bool isManifestSeeded() const { return manifestSeeded_; }

  // Records that the first sync against this pairing has now happened.
  // No-op, and no NVS write, once it is already set.
  void setManifestSeeded();

  // Forgets the pairing (token/deviceId/deviceName/accountEmail, and the
  // first-sync seed marker above) but keeps the server URL override -- what
  // "Unlink Device" in Settings does.
  void clearPairing();

  // Forgets everything, including the server URL override -- full reset.
  void clearAll();

 private:
  SyncCredentialStore() = default;

  std::string serverUrl_;
  std::string accessToken_;
  std::string deviceId_;
  std::string deviceName_;
  std::string accountEmail_;
  bool manifestSeeded_ = false;
};

// Helper macro to access the store, matching WIFI_STORE / KOREADER_STORE.
#define SYNC_STORE SyncCredentialStore::getInstance()
