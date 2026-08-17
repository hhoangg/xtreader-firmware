#include <gtest/gtest.h>

#include <limits>

#include "HeartbeatPayload.h"

namespace {

using heartbeat_payload::computeSdFreeBytes;
using heartbeat_payload::computeSdTotalBytes;

constexpr uint64_t OMIT = std::numeric_limits<uint64_t>::max();

TEST(ComputeSdTotalBytes, PassesThroughARealCapacity) { EXPECT_EQ(computeSdTotalBytes(32ULL << 30), 32ULL << 30); }

TEST(ComputeSdTotalBytes, OmitsWhenTheCardWasNeverMounted) {
  // SDCardManager::sdTotalBytes() reports 0 before begin() succeeds -- must
  // not be sent as a literal 0-byte card.
  EXPECT_EQ(computeSdTotalBytes(0), OMIT);
}

TEST(ComputeSdFreeBytes, SubtractsUsedFromTotal) { EXPECT_EQ(computeSdFreeBytes(1000, 400), 600u); }

TEST(ComputeSdFreeBytes, OmitsWhenTheCardWasNeverMounted) { EXPECT_EQ(computeSdFreeBytes(0, 0), OMIT); }

TEST(ComputeSdFreeBytes, OmitsWhenUsedExceedsTotal) {
  // A stale used-byte cache racing a mount/unmount can momentarily read
  // higher than total -- must not wrap around to a huge "free" value.
  EXPECT_EQ(computeSdFreeBytes(1000, 1500), OMIT);
}

TEST(ComputeSdFreeBytes, ZeroFreeOnAFullCardIsReportedAsZeroNotOmitted) {
  // A genuinely full card is real information, not an unknown -- must not
  // collide with the OMIT sentinel.
  EXPECT_EQ(computeSdFreeBytes(1000, 1000), 0u);
}

}  // namespace
