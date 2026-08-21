#include <gtest/gtest.h>

#include <cstdint>

#include "SyncTriggerPolicy.h"

namespace {

using sync_trigger::shouldAttemptLibraryWifiConnect;
using sync_trigger::shouldAutoSync;
using sync_trigger::shouldDeliverPendingBookFinished;
using sync_trigger::shouldSyncBeforeSleep;
using sync_trigger::shouldSyncWallpapers;
using sync_trigger::WALLPAPER_SYNC_BOOT_INTERVAL;

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

TEST(ShouldAttemptLibraryWifiConnect, FiresWhenPairedDisconnectedAndNotYetAttempted) {
  EXPECT_TRUE(shouldAttemptLibraryWifiConnect(/*paired=*/true, /*wifiConnected=*/false,
                                              /*alreadyAttemptedThisBoot=*/false));
}

TEST(ShouldAttemptLibraryWifiConnect, NeverFiresWhenUnpaired) {
  // An unpaired device must behave exactly as today: no radio, no delay, no indicator.
  EXPECT_FALSE(shouldAttemptLibraryWifiConnect(false, false, false));
}

TEST(ShouldAttemptLibraryWifiConnect, NothingToGainWhenAlreadyConnected) {
  EXPECT_FALSE(shouldAttemptLibraryWifiConnect(true, /*wifiConnected=*/true, false));
}

TEST(ShouldAttemptLibraryWifiConnect, AtMostOncePerBoot) {
  EXPECT_FALSE(shouldAttemptLibraryWifiConnect(true, false, /*alreadyAttemptedThisBoot=*/true));
}

TEST(ShouldAttemptLibraryWifiConnect, UnpairedConnectedAndAlreadyAttemptedStillFalse) {
  EXPECT_FALSE(shouldAttemptLibraryWifiConnect(false, true, true));
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

TEST(WallpaperSyncTrigger, FiresOnceTheBootIntervalHasPassed) {
  EXPECT_TRUE(shouldSyncWallpapers(/*paired=*/true, /*wifiConnected=*/true, /*alreadyAttemptedThisBoot=*/false,
                                   WALLPAPER_SYNC_BOOT_INTERVAL));
}

TEST(WallpaperSyncTrigger, FiresOnADeviceThatHasNeverSynced) {
  // UINT16_MAX is CrossPointState's default, and also what a state.json
  // written before the field existed reads back as -- a freshly paired
  // reader must get its wallpapers now, not in eight boots.
  EXPECT_TRUE(shouldSyncWallpapers(true, true, false, UINT16_MAX));
}

TEST(WallpaperSyncTrigger, WaitsOutTheCadence) {
  for (uint16_t boots = 0; boots < WALLPAPER_SYNC_BOOT_INTERVAL; boots++) {
    EXPECT_FALSE(shouldSyncWallpapers(true, true, false, boots)) << boots;
  }
}

TEST(WallpaperSyncTrigger, NeverFiresUnpaired) {
  EXPECT_FALSE(shouldSyncWallpapers(/*paired=*/false, true, false, UINT16_MAX));
}

TEST(WallpaperSyncTrigger, NeverBringsWifiUpItself) {
  // Same rule as shouldAutoSync(): it rides on a connection something else
  // established, never one it pays for.
  EXPECT_FALSE(shouldSyncWallpapers(true, /*wifiConnected=*/false, false, UINT16_MAX));
}

TEST(WallpaperSyncTrigger, NeverFiresTwiceInOneBoot) {
  EXPECT_FALSE(shouldSyncWallpapers(true, true, /*alreadyAttemptedThisBoot=*/true, UINT16_MAX));
}

}  // namespace
