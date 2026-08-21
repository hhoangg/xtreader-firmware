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

/**
 * Singleton holding the KOSync settings that shape a progress upload. The
 * credential itself is provisioned by device pairing and lives in NVS (see
 * below); only these two non-secret knobs are persisted to the SD card.
 */

class KOReaderCredentialStore : public PersistableStore<KOReaderCredentialStore> {
 private:
  DocumentMatchMethod matchMethod = DocumentMatchMethod::FILENAME;  // Default to filename for compatibility
  bool sendMetadata = false;                                        // Send document metadata with progress sync

  // Private constructor for singleton
  KOReaderCredentialStore() = default;
  ~KOReaderCredentialStore() = default;

  friend class PersistableStore<KOReaderCredentialStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/koreader.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Document matching method
  void setMatchMethod(DocumentMatchMethod method);
  DocumentMatchMethod getMatchMethod() const { return matchMethod; }

  // Send metadata setting
  void setSendMetadata(bool enabled);
  bool getSendMetadata() const { return sendMetadata; }

  // --- Provisioned credential (from device pairing) -----------------------
  // Set by SyncPairingActivity on a successful pairing; cleared by
  // SyncSettingsActivity on unlink. Deliberately NOT part of toJson()/fromJson()
  // above, which writes to the SD card (readable in any card reader): this is a
  // bearer-token-like secret minted by the pairing server. It lives in NVS
  // instead, the same partition SyncCredentialStore's own pairing token uses,
  // and every getter re-reads it live (no in-memory cache) so an unlink or a
  // re-pair takes effect immediately without a reboot.
  //
  // getMatchMethod()/getSendMetadata() above are untouched by this: they are
  // not secrets, and SyncPairingActivity sets them directly (still
  // SD-persisted) instead.
  void setProvisionedCredential(const std::string& username, const std::string& keyMd5, const std::string& serverUrl);
  void clearProvisionedCredential();
  bool hasProvisionedCredential() const;
  std::string getProvisionedUsername() const;
  std::string getProvisionedKeyMd5() const;
  std::string getProvisionedServerUrl() const;

  // --- Effective credential: the accessors every caller should use --------
  // Pairing is the only way a credential ever gets here, so these are thin
  // wrappers over the provisioned values. Callers go through them rather than
  // the getProvisioned* getters so the "is progress sync configured at all"
  // check stays in one place. effectiveKeyMd5() returns the provisioned key
  // as-is (it is already MD5-shaped, see setProvisionedCredential()'s caller)
  // rather than hashing it again.
  bool hasEffectiveCredentials() const { return hasProvisionedCredential(); }
  std::string effectiveUsername() const { return getProvisionedUsername(); }
  std::string effectiveKeyMd5() const { return getProvisionedKeyMd5(); }
  std::string effectiveBaseUrl() const { return getProvisionedServerUrl(); }
  // A provisioned credential always targets a crosspoint-sync-compatible
  // server by construction (it came from that server's own pairing response),
  // regardless of self-hosted domain, so the protocol extensions are always
  // safe to send.
  bool effectiveUsesCrossPointSyncServer() const { return hasProvisionedCredential(); }
};

// Helper macro to access credential store
#define KOREADER_STORE KOReaderCredentialStore::getInstance()
