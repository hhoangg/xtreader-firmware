#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

// Document matching method for KOReader sync
enum class DocumentMatchMethod : uint8_t {
  FILENAME = 0,  // Match by filename (simpler, works across different file sources)
  BINARY = 1,    // Match by partial MD5 of file content (more accurate, but files must be identical)
};

// How manual "Sync Progress" resolves differences after fetching remote progress.
enum class KOReaderSyncBehavior : uint8_t {
  ASK_EVERY_TIME = 0,  // Preserve legacy behavior: always show Apply/Upload choices.
  SMART = 1,           // Auto-resolve simple cases using furthest progress.
};

/**
 * Singleton class for storing KOReader sync credentials on the SD card.
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON (not cryptographically secure,
 * but prevents casual reading and ties credentials to the specific device).
 */

class KOReaderCredentialStore : public PersistableStore<KOReaderCredentialStore> {
 private:
  std::string username;
  std::string password;
  std::string serverUrl;                                            // Custom sync server URL (empty = default)
  DocumentMatchMethod matchMethod = DocumentMatchMethod::FILENAME;  // Default to filename for compatibility
  bool sendMetadata = false;                                        // Send document metadata with progress sync
  KOReaderSyncBehavior syncBehavior = KOReaderSyncBehavior::SMART;

  // Private constructor for singleton
  KOReaderCredentialStore() = default;
  ~KOReaderCredentialStore() = default;

  friend class PersistableStore<KOReaderCredentialStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/koreader.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Credential management
  void setCredentials(const std::string& user, const std::string& pass);
  const std::string& getUsername() const { return username; }
  const std::string& getPassword() const { return password; }

  // Get MD5 hash of password for API authentication
  std::string getMd5Password() const;

  // Check if credentials are set
  bool hasCredentials() const;

  // Clear credentials
  void clearCredentials();

  // Server URL management
  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const { return serverUrl; }

  // Get base URL for API calls (with http:// normalization if no protocol, falls back to default)
  std::string getBaseUrl() const;

  // Whether API calls target the CrossPoint sync server that supports protocol extensions.
  bool usesCrossPointSyncServer() const;

  // Document matching method
  void setMatchMethod(DocumentMatchMethod method);
  DocumentMatchMethod getMatchMethod() const { return matchMethod; }

  // Send metadata setting
  void setSendMetadata(bool enabled);
  bool getSendMetadata() const { return sendMetadata; }

  // Sync behavior
  void setSyncBehavior(KOReaderSyncBehavior behavior);
  KOReaderSyncBehavior getSyncBehavior() const { return syncBehavior; }

  // --- Provisioned credential (from device pairing) -----------------------
  // Set by SyncPairingActivity on a successful pairing; cleared by
  // SyncSettingsActivity on unlink. Deliberately NOT part of the
  // username/password/toJson()/fromJson() above: those are obfuscated and
  // written to the SD card (readable in any card reader), which is fine for
  // a manually-typed KOReader password but not for a bearer-token-like
  // secret minted by the pairing server. This lives in NVS instead, the same
  // partition SyncCredentialStore's own pairing token uses, and every getter
  // re-reads it live (no in-memory cache) so an unlink or a re-pair takes
  // effect immediately without a reboot.
  //
  // KOReaderSyncClient checks hasProvisionedCredential() first and prefers
  // these three over getUsername()/getMd5Password()/getBaseUrl() whenever
  // it is set, so a paired device needs no manual KOReader Sync setup.
  // getMatchMethod() above is untouched by this: it is not a secret, and
  // SyncPairingActivity sets it directly (still SD-persisted) instead.
  void setProvisionedCredential(const std::string& username, const std::string& keyMd5, const std::string& serverUrl);
  void clearProvisionedCredential();
  bool hasProvisionedCredential() const;
  std::string getProvisionedUsername() const;
  std::string getProvisionedKeyMd5() const;
  std::string getProvisionedServerUrl() const;

  // --- Effective credential: the precedence every caller should use -------
  // Provisioned when set, otherwise the manual fields above. KOReaderSyncClient
  // and KOReaderSyncActivity go through these exclusively (never the raw
  // manual/provisioned getters directly) so a paired device needs no separate
  // "sync progress" setup. effectiveKeyMd5() returns the provisioned key as-is
  // (it is already MD5-shaped, see setProvisionedCredential()'s caller) rather
  // than hashing it again.
  bool hasEffectiveCredentials() const { return hasProvisionedCredential() || hasCredentials(); }
  std::string effectiveUsername() const {
    return hasProvisionedCredential() ? getProvisionedUsername() : getUsername();
  }
  std::string effectiveKeyMd5() const { return hasProvisionedCredential() ? getProvisionedKeyMd5() : getMd5Password(); }
  std::string effectiveBaseUrl() const { return hasProvisionedCredential() ? getProvisionedServerUrl() : getBaseUrl(); }
  // A provisioned credential always targets a crosspoint-sync-compatible
  // server by construction (it came from that server's own pairing
  // response), regardless of self-hosted domain -- unlike the manual path,
  // which has to compare hostnames because the user could have typed anything.
  bool effectiveUsesCrossPointSyncServer() const { return hasProvisionedCredential() || usesCrossPointSyncServer(); }
};

// Helper macro to access credential store
#define KOREADER_STORE KOReaderCredentialStore::getInstance()
