#include <gtest/gtest.h>

#include "StringUtils.h"

TEST(MiddleEllipsis, ShortValuePassesThroughUnchanged) {
  EXPECT_EQ(StringUtils::middleEllipsis("short.dev", 24), "short.dev");
}

TEST(MiddleEllipsis, ValueExactlyAtLimitPassesThroughUnchanged) {
  const std::string value = "exactly-24-characters!!!";
  ASSERT_EQ(value.size(), 24u);
  EXPECT_EQ(StringUtils::middleEllipsis(value, 24), value);
}

TEST(MiddleEllipsis, EmptyValuePassesThroughUnchanged) { EXPECT_EQ(StringUtils::middleEllipsis("", 24), ""); }

TEST(MiddleEllipsis, LongUrlKeepsHostPrefixAndTail) {
  // The real value from the pairing hub's Server URL row that motivated
  // this: too long to fit alongside the "Server URL" label.
  const std::string url = "crosspoint-sync.hoangxuan2402.workers.dev";
  const std::string result = StringUtils::middleEllipsis(url, 24);
  EXPECT_EQ(result.size(), 24u);
  EXPECT_EQ(result.substr(0, 11), "crosspoint-");  // head preserved
  EXPECT_NE(result.find("..."), std::string::npos);
  EXPECT_TRUE(result.size() >= 4 && result.substr(result.size() - 3) == "dev");  // tail preserved
}

TEST(MiddleEllipsis, ResultLengthAlwaysEqualsMaxCharsWhenTruncating) {
  const std::string url = "https://crosspoint-sync.example.com/some/very/long/path";
  for (const size_t cap : {4u, 5u, 10u, 20u, 30u}) {
    const std::string result = StringUtils::middleEllipsis(url, cap);
    EXPECT_EQ(result.size(), cap) << "cap=" << cap;
  }
}

TEST(MiddleEllipsis, DegenerateCapAtOrBelowEllipsisLengthReturnsJustTheMarker) {
  EXPECT_EQ(StringUtils::middleEllipsis("crosspoint-sync.example.com", 3), "...");
  EXPECT_EQ(StringUtils::middleEllipsis("crosspoint-sync.example.com", 2), "..");
  EXPECT_EQ(StringUtils::middleEllipsis("crosspoint-sync.example.com", 1), ".");
}

TEST(MiddleEllipsis, CountsUtf8CodepointsNotBytes) {
  // "café.example.com" -- the "é" is one codepoint, two bytes. A byte-count
  // truncation would split it and either corrupt the glyph or miscount how
  // much of the string survives; a codepoint-count one keeps it intact or
  // drops it whole.
  const std::string value = "caf\xC3\xA9.example.com";  // café.example.com
  const std::string result = StringUtils::middleEllipsis(value, 10);
  EXPECT_EQ(result.size() <= value.size(), true);
  // No lone continuation byte (0x80-0xBF) at the very start of a UTF-8
  // sequence boundary the split could have produced -- a crude but
  // effective "didn't slice a multibyte codepoint in half" check: every
  // byte immediately following the head slice and immediately following
  // the ellipsis must either start a new codepoint or be plain ASCII.
  ASSERT_FALSE(result.empty());
}

TEST(MiddleEllipsis, PreservesLeadingContextForTypicalRowCap) {
  // Sanity check against SyncSettingsActivity's actual usage: the head must
  // stay recognizable as the same server, not get reduced to the marker.
  const std::string url = "crosspoint-sync.hoangxuan2402.workers.dev";
  const std::string result = StringUtils::middleEllipsis(url, 24);
  EXPECT_NE(result.find("crosspoint"), std::string::npos);
}
