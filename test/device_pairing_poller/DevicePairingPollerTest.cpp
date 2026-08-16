#include <gtest/gtest.h>

#include "DevicePairingPoller.h"

TEST(DevicePairingPoller, StartEntersWaitingAndIsNotDueImmediately) {
  DevicePairingPoller poller;
  poller.start(/*nowMs=*/0, /*intervalSec=*/15, /*expiresInSec=*/300);
  EXPECT_EQ(poller.state(), DevicePairingPollState::WAITING);
  EXPECT_FALSE(poller.dueForPoll(0));
  EXPECT_FALSE(poller.dueForPoll(14999));
}

TEST(DevicePairingPoller, BecomesDueOnceIntervalElapses) {
  DevicePairingPoller poller;
  poller.start(0, 15, 300);
  EXPECT_TRUE(poller.dueForPoll(15000));
  EXPECT_TRUE(poller.dueForPoll(20000));  // still due until fed a result
}

TEST(DevicePairingPoller, AuthorizationPendingReschedulesOneIntervalOut) {
  DevicePairingPoller poller;
  poller.start(0, 15, 300);
  ASSERT_TRUE(poller.dueForPoll(15000));
  poller.onPollResult(15000, DevicePairingPollOutcome::AUTHORIZATION_PENDING);
  EXPECT_EQ(poller.state(), DevicePairingPollState::WAITING);
  EXPECT_FALSE(poller.dueForPoll(29999));
  EXPECT_TRUE(poller.dueForPoll(30000));
}

TEST(DevicePairingPoller, TransportErrorRetriesLikeAuthorizationPending) {
  DevicePairingPoller poller;
  poller.start(0, 15, 300);
  ASSERT_TRUE(poller.dueForPoll(15000));
  poller.onPollResult(15000, DevicePairingPollOutcome::TRANSPORT_ERROR);
  EXPECT_EQ(poller.state(), DevicePairingPollState::WAITING);
  EXPECT_FALSE(poller.dueForPoll(29999));
  EXPECT_TRUE(poller.dueForPoll(30000));
}

TEST(DevicePairingPoller, SlowDownBacksOffTheIntervalAndKeepsItBackedOff) {
  DevicePairingPoller poller;
  poller.start(0, 15, 300);
  EXPECT_EQ(poller.intervalSec(), 15u);
  ASSERT_TRUE(poller.dueForPoll(15000));
  poller.onPollResult(15000, DevicePairingPollOutcome::SLOW_DOWN);
  EXPECT_EQ(poller.intervalSec(), 20u);  // RFC 8628: back off by (at least) a few seconds
  EXPECT_FALSE(poller.dueForPoll(34999));
  EXPECT_TRUE(poller.dueForPoll(35000));

  // A second slow_down keeps compounding rather than resetting.
  poller.onPollResult(35000, DevicePairingPollOutcome::SLOW_DOWN);
  EXPECT_EQ(poller.intervalSec(), 25u);
}

TEST(DevicePairingPoller, AccessDeniedIsTerminal) {
  DevicePairingPoller poller;
  poller.start(0, 15, 300);
  ASSERT_TRUE(poller.dueForPoll(15000));
  poller.onPollResult(15000, DevicePairingPollOutcome::ACCESS_DENIED);
  EXPECT_EQ(poller.state(), DevicePairingPollState::DENIED);
  // A terminal state never asks the caller to poll again.
  EXPECT_FALSE(poller.dueForPoll(999999));
  // And is not perturbed by a stray extra onPollResult call.
  poller.onPollResult(999999, DevicePairingPollOutcome::AUTHORIZATION_PENDING);
  EXPECT_EQ(poller.state(), DevicePairingPollState::DENIED);
}

TEST(DevicePairingPoller, ExpiredTokenFromServerIsTerminal) {
  DevicePairingPoller poller;
  poller.start(0, 15, 300);
  ASSERT_TRUE(poller.dueForPoll(15000));
  poller.onPollResult(15000, DevicePairingPollOutcome::EXPIRED_TOKEN);
  EXPECT_EQ(poller.state(), DevicePairingPollState::EXPIRED);
  EXPECT_FALSE(poller.dueForPoll(999999));
}

TEST(DevicePairingPoller, LocalTimeoutGivesUpWithoutAnyServerResponse) {
  // "Give up when expiresIn elapses rather than polling forever" -- the
  // poller must expire on its own even if the caller never gets another
  // poll result back (e.g. Wi-Fi died and no request was ever sent again).
  // Simulate a caller that dutifully re-polls (and reschedules) right up to
  // the edge of the expiry window, then stops getting responses.
  DevicePairingPoller poller;
  poller.start(0, 15, 300);
  for (uint32_t t = 15000; t < 300000; t += 15000) {
    ASSERT_TRUE(poller.dueForPoll(t));
    poller.onPollResult(t, DevicePairingPollOutcome::AUTHORIZATION_PENDING);
  }
  EXPECT_EQ(poller.state(), DevicePairingPollState::WAITING);
  EXPECT_FALSE(poller.dueForPoll(300000));
  EXPECT_EQ(poller.state(), DevicePairingPollState::EXPIRED);
}

TEST(DevicePairingPoller, SecondsRemainingCountsDownAndFloorsAtZero) {
  DevicePairingPoller poller;
  poller.start(0, 15, 300);
  EXPECT_EQ(poller.secondsRemaining(0), 300u);
  EXPECT_EQ(poller.secondsRemaining(150000), 150u);
  EXPECT_EQ(poller.secondsRemaining(300000), 0u);
  EXPECT_EQ(poller.secondsRemaining(999999), 0u);
}

TEST(DevicePairingPoller, ZeroIntervalAndExpiryFallBackToApiDefaults) {
  // A server response that omits interval/expiresIn (both 0 in the parsed
  // struct) must not spin dueForPoll() true forever or expire immediately.
  DevicePairingPoller poller;
  poller.start(0, /*intervalSec=*/0, /*expiresInSec=*/0);
  EXPECT_EQ(poller.intervalSec(), 15u);
  EXPECT_EQ(poller.secondsRemaining(0), 300u);
}
