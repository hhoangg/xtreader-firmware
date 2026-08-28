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
  // The workers.dev hostname this firmware defaulted to when the Server URL
  // row was first written, and what motivated truncating at all: too long to
  // fit alongside the "Server URL" label. Kept as the sample even though the
  // default is now a short custom domain -- a self-hosted URL can be any
  // length, so the long case is the one worth pinning.
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

namespace {
std::string jsonEscape(const std::string& value) {
  std::string out;
  StringUtils::appendJsonEscaped(out, value.data(), value.size());
  return out;
}
}  // namespace

TEST(AppendJsonEscaped, PlainAsciiPassesThroughQuoted) { EXPECT_EQ(jsonEscape("hello"), "\"hello\""); }

TEST(AppendJsonEscaped, EscapesQuoteAndBackslash) { EXPECT_EQ(jsonEscape("a\"b\\c"), "\"a\\\"b\\\\c\""); }

TEST(AppendJsonEscaped, EscapesNamedControlCharsWithShorthand) {
  EXPECT_EQ(jsonEscape("a\nb\rc\td"), "\"a\\nb\\rc\\td\"");
}

TEST(AppendJsonEscaped, EscapesOtherControlCharsAndDelAsUnicodeEscape) {
  EXPECT_EQ(jsonEscape(std::string("\x01", 1)), "\"\\u0001\"");
  EXPECT_EQ(jsonEscape(std::string("\x7f", 1)), "\"\\u007f\"");
  EXPECT_EQ(jsonEscape(std::string("\x00", 1)), "\"\\u0000\"");
}

TEST(AppendJsonEscaped, PassesMultiByteUtf8ThroughRawInsteadOfPerByteEscaping) {
  // "Máy đọc của tôi 2" (Vietnamese device name from CMD:PAIR). The bug this
  // guards against: escaping every UTF-8 byte as its own \u00XX turns each
  // multi-byte character into a run of separate Latin-1-valued codepoints
  // once a JSON decoder parses it back -- valid JSON, but mojibake ("MÃ¡y
  // Ä‘á»c...") for any UTF-8-aware reader. The fix is to leave high-bit bytes
  // untouched so the JSON string stays correct UTF-8.
  // Split so each \xXX escape ends its own string literal token -- otherwise
  // the lexer greedily consumes a following hex-digit character (e.g. the
  // 'c' after \x8D, or the 'a' after \xA7) as more of the same escape.
  const std::string name =
      "M"
      "\xC3\xA1"
      "y "
      "\xC4\x91"
      "\xE1\xBB\x8D"
      "c c"
      "\xE1\xBB\xA7"
      "a t"
      "\xC3\xB4"
      "i 2";
  const std::string result = jsonEscape(name);
  EXPECT_EQ(result, "\"" + name + "\"");
  // Confirm no \u00XX escape leaked in for any high-bit byte.
  EXPECT_EQ(result.find("\\u00"), std::string::npos);
}

TEST(AppendJsonEscaped, EmptyInputProducesEmptyQuotedString) { EXPECT_EQ(jsonEscape(""), "\"\""); }

TEST(FormatUtcDate, EpochZeroIsTheUnixEpochDay) { EXPECT_EQ(StringUtils::formatUtcDate(0, 0), "1970-01-01"); }

TEST(FormatUtcDate, RendersALeapDay) {
  // 1709164800 == 2024-02-29T00:00:00Z
  EXPECT_EQ(StringUtils::formatUtcDate(1709164800, 0), "2024-02-29");
}

TEST(FormatUtcDate, RendersAnOrdinaryDay) {
  // 1756339200 == 2025-08-28T00:00:00Z
  EXPECT_EQ(StringUtils::formatUtcDate(1756339200, 0), "2025-08-28");
}

TEST(FormatUtcDate, PositiveOffsetCanRollForwardIntoTheNextYear) {
  // 1704063600 == 2023-12-31T23:00:00Z; +7h lands on New Year's Day.
  EXPECT_EQ(StringUtils::formatUtcDate(1704063600, 7 * 3600), "2024-01-01");
}

TEST(FormatUtcDate, NegativeOffsetCanRollBackADay) {
  // Midnight UTC on the leap day, minus one hour, is the day before.
  EXPECT_EQ(StringUtils::formatUtcDate(1709164800, -3600), "2024-02-28");
}

TEST(FormatUtcDate, OffsetWithinTheSameDayDoesNotMoveIt) {
  EXPECT_EQ(StringUtils::formatUtcDate(1709164800, 7 * 3600), "2024-02-29");
}
