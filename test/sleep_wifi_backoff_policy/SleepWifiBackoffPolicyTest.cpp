#include <gtest/gtest.h>

#include "SleepWifiBackoffPolicy.h"

namespace {

using sleep_wifi_backoff::afterAttempt;
using sleep_wifi_backoff::afterSkippedAttempt;
using sleep_wifi_backoff::shouldAttempt;
using sleep_wifi_backoff::State;
using sleep_wifi_backoff::MAX_SKIP;

TEST(SleepWifiBackoffPolicy, FreshStateAlwaysAttempts) {
  EXPECT_TRUE(shouldAttempt(State{}));
}

TEST(SleepWifiBackoffPolicy, NonZeroSkipsRemainingBlocksAttempt) {
  EXPECT_FALSE(shouldAttempt(State{/*consecutiveFailures=*/1, /*skipsRemaining=*/1}));
}

TEST(SleepWifiBackoffPolicy, FirstFailureSchedulesOneSkip) {
  const State next = afterAttempt(State{}, /*wifiConnected=*/false);
  EXPECT_EQ(next.consecutiveFailures, 1);
  EXPECT_EQ(next.skipsRemaining, 1);
}

TEST(SleepWifiBackoffPolicy, SecondConsecutiveFailureSchedulesTwoSkips) {
  const State afterFirst = afterAttempt(State{}, false);
  // The skip from afterFirst must run out before the caller attempts again.
  const State next = afterAttempt(afterFirst, /*wifiConnected=*/false);
  EXPECT_EQ(next.consecutiveFailures, 2);
  EXPECT_EQ(next.skipsRemaining, 2);
}

TEST(SleepWifiBackoffPolicy, SuccessResetsStateEntirely) {
  const State backedOff{/*consecutiveFailures=*/4, /*skipsRemaining=*/0};
  const State next = afterAttempt(backedOff, /*wifiConnected=*/true);
  EXPECT_EQ(next.consecutiveFailures, 0);
  EXPECT_EQ(next.skipsRemaining, 0);
}

TEST(SleepWifiBackoffPolicy, SkipCountIsCappedAtMaxSkip) {
  State state{};
  for (int i = 0; i < 20; i++) {
    state = afterAttempt(state, false);
  }
  EXPECT_EQ(state.consecutiveFailures, MAX_SKIP);
  EXPECT_EQ(state.skipsRemaining, MAX_SKIP);
}

TEST(SleepWifiBackoffPolicy, SkippedAttemptConsumesExactlyOneSkipAndKeepsFailureCount) {
  const State state{/*consecutiveFailures=*/3, /*skipsRemaining=*/3};
  const State next = afterSkippedAttempt(state);
  EXPECT_EQ(next.consecutiveFailures, 3);
  EXPECT_EQ(next.skipsRemaining, 2);
}

TEST(SleepWifiBackoffPolicy, SkippedAttemptNeverGoesNegative) {
  const State state{/*consecutiveFailures=*/0, /*skipsRemaining=*/0};
  const State next = afterSkippedAttempt(state);
  EXPECT_EQ(next.skipsRemaining, 0);
}

TEST(SleepWifiBackoffPolicy, EventuallyRetriesAfterRepeatedFailures) {
  // Simulate a device stuck with no Wi-Fi for many sleeps: it must never
  // permanently stop attempting -- shouldAttempt() must go true again after
  // enough skipped sleeps, every cycle.
  State state{};
  for (int cycle = 0; cycle < 3; cycle++) {
    EXPECT_TRUE(shouldAttempt(state));
    state = afterAttempt(state, false);
    int skipped = 0;
    while (!shouldAttempt(state)) {
      state = afterSkippedAttempt(state);
      skipped++;
      ASSERT_LT(skipped, 100) << "back-off never releases the attempt";
    }
  }
}

}  // namespace
