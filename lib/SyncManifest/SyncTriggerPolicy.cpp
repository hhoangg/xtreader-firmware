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

}  // namespace sync_trigger
