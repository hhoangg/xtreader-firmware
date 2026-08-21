#include "SyncTriggerPolicy.h"

namespace sync_trigger {

bool shouldAutoSync(const bool paired, const bool wifiConnected, const bool alreadyAttemptedThisBoot) {
  return paired && wifiConnected && !alreadyAttemptedThisBoot;
}

bool shouldAttemptLibraryWifiConnect(const bool paired, const bool wifiConnected, const bool alreadyAttemptedThisBoot) {
  return paired && !wifiConnected && !alreadyAttemptedThisBoot;
}

bool shouldDeliverPendingBookFinished(const bool hasPending, const bool paired, const bool wifiConnected,
                                      const bool alreadyAttemptedThisVisit) {
  return hasPending && paired && wifiConnected && !alreadyAttemptedThisVisit;
}

bool shouldSyncBeforeSleep(const bool paired, const bool isReaderActivity, const bool dirty) {
  return paired && isReaderActivity && dirty;
}

bool shouldSyncWallpapers(const bool paired, const bool wifiConnected, const bool alreadyAttemptedThisBoot,
                          const uint16_t bootsSinceLastSync, const uint32_t heartbeatWallpaperRevision,
                          const uint32_t lastSyncedWallpaperRevision) {
  // 0 is "unknown", never a real revision -- so it can only ever withhold
  // this reason, not create one.
  const bool revisionChanged =
      heartbeatWallpaperRevision != 0 && heartbeatWallpaperRevision != lastSyncedWallpaperRevision;
  return paired && wifiConnected && !alreadyAttemptedThisBoot &&
         (bootsSinceLastSync >= WALLPAPER_SYNC_BOOT_INTERVAL || revisionChanged);
}

}  // namespace sync_trigger
