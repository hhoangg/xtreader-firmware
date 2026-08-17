#include <gtest/gtest.h>

#include "SyncTriggerPolicy.h"

namespace {

using sync_trigger::shouldAutoSync;
using sync_trigger::shouldDeliverPendingBookFinished;
using sync_trigger::shouldSyncBeforeSleep;

TEST(SyncTriggerPolicy, FiresWhenPairedConnectedAndNotYetAttempted) {
  EXPECT_TRUE(shouldAutoSync(/*paired=*/true, /*wifiConnected=*/true, /*alreadyAttemptedThisBoot=*/false));
}

TEST(SyncTriggerPolicy, NeverFiresWhenUnpaired) { EXPECT_FALSE(shouldAutoSync(false, true, false)); }

TEST(SyncTriggerPolicy, NeverBringsWifiUpItself) {
  // paired and never attempted, but WiFi is not already connected -- must
  // not fire (the whole point: only sync if WiFi is already up).
  EXPECT_FALSE(shouldAutoSync(true, false, false));
}

TEST(SyncTriggerPolicy, AtMostOncePerBoot) {
  EXPECT_FALSE(shouldAutoSync(true, true, /*alreadyAttemptedThisBoot=*/true));
}

TEST(SyncTriggerPolicy, UnpairedAndDisconnectedAndAlreadyAttemptedStillFalse) {
  EXPECT_FALSE(shouldAutoSync(false, false, true));
}

TEST(DeliverPendingBookFinished, DeliversWhenPendingPairedConnectedAndNotYetAttemptedThisVisit) {
  EXPECT_TRUE(shouldDeliverPendingBookFinished(/*hasPending=*/true, /*paired=*/true, /*wifiConnected=*/true,
                                               /*alreadyAttemptedThisVisit=*/false));
}

TEST(DeliverPendingBookFinished, NothingToDeliverWhenNoneIsPending) {
  // Paired, connected, fresh visit -- but nothing pending, so no attempt.
  EXPECT_FALSE(shouldDeliverPendingBookFinished(false, true, true, false));
}

TEST(DeliverPendingBookFinished, NeverFiresWhenUnpaired) {
  EXPECT_FALSE(shouldDeliverPendingBookFinished(true, false, true, false));
}

TEST(DeliverPendingBookFinished, NeverBringsWifiUpItself) {
  EXPECT_FALSE(shouldDeliverPendingBookFinished(true, true, false, false));
}

TEST(DeliverPendingBookFinished, AtMostOncePerVisitEvenWithSomethingStillPending) {
  // A pending event survives a failed attempt (the caller leaves it set to
  // retry on a later visit), but must not be retried again within the same
  // visit -- see the header comment on why (repeated blocking network calls
  // on every render pass while the user sits on Home).
  EXPECT_FALSE(shouldDeliverPendingBookFinished(true, true, true, /*alreadyAttemptedThisVisit=*/true));
}

TEST(SyncBeforeSleep, FiresWhenPairedInReaderAndDirty) {
  EXPECT_TRUE(shouldSyncBeforeSleep(/*paired=*/true, /*isReaderActivity=*/true, /*dirty=*/true));
}

TEST(SyncBeforeSleep, NeverFiresWhenUnpaired) {
  // An unpaired device must sleep exactly as it does today: no delay, no screen.
  EXPECT_FALSE(shouldSyncBeforeSleep(false, true, true));
}

TEST(SyncBeforeSleep, NeverFiresOutsideTheReader) { EXPECT_FALSE(shouldSyncBeforeSleep(true, false, true)); }

TEST(SyncBeforeSleep, NeverFiresWhenClean) {
  // Book open, nothing read yet -- nothing new to send, so no WiFi/TLS cost.
  EXPECT_FALSE(shouldSyncBeforeSleep(true, true, /*dirty=*/false));
}

TEST(SyncBeforeSleep, UnpairedOutsideReaderAndCleanStillFalse) {
  EXPECT_FALSE(shouldSyncBeforeSleep(false, false, false));
}

}  // namespace
