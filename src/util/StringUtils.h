#pragma once

#include <cctype>
#include <cstdint>
#include <string>

namespace StringUtils {

/**
 * Case-insensitive ASCII strcmp. Returns <0, 0, or >0 like strcmp, comparing
 * each byte by its lowercased value.
 *
 * Used wherever data is sorted case-insensitively: StarDict indexes (including
 * wiktionary-derived dictionaries) and the on-disk dictionary folder list.
 * Plain strcmp would land a binary search on the wrong page for any word whose
 * alphabetic neighbourhood contains mixed-case boundaries.
 *
 * Inline (header) because Dictionary's binary search calls it per comparison
 * step; a cross-TU call here would defeat inlining on a hot path.
 */
inline int asciiCaseCmp(const char* a, const char* b) {
  while (*a && *b) {
    int diff = std::tolower(static_cast<unsigned char>(*a)) - std::tolower(static_cast<unsigned char>(*b));
    if (diff != 0) return diff;
    ++a;
    ++b;
  }
  return std::tolower(static_cast<unsigned char>(*a)) - std::tolower(static_cast<unsigned char>(*b));
}

/**
 * Sanitize a string for use as a filename.
 * Replaces invalid characters with underscores, trims spaces/dots,
 * and limits length to maxBytes bytes.
 */
std::string sanitizeFilename(const std::string& name, size_t maxBytes = 100);

/**
 * Shortens `value` to at most maxChars characters (UTF-8 codepoints, not
 * bytes) by replacing a middle span with "...", keeping the head and tail
 * intact. No-op if value already fits within maxChars.
 *
 * For values where both ends carry meaning -- a URL's host at the front and
 * its distinguishing tail at the back, e.g. -- unlike a plain trailing
 * ellipsis, which for a hostname keeps only the generic prefix and throws
 * away exactly the part that would tell two servers apart.
 *
 * This is a character-count heuristic, not a pixel measurement (this
 * codebase's list rows have no built-in value-truncation of their own — see
 * SyncSettingsActivity's Server URL row, the case this was written for), so
 * pick maxChars conservatively for the font/row it's going into.
 */
std::string middleEllipsis(const std::string& value, size_t maxChars);

/**
 * Appends `data` (`len` bytes) to `out` as a double-quoted JSON string
 * literal, escaping only what JSON requires: the quote, the backslash, and
 * C0 control characters (using the named \n/\r/\t shorthands where they
 * exist, \u00XX otherwise).
 *
 * `data` is assumed to already be valid UTF-8 (device names, file paths, log
 * messages) -- bytes with the high bit set are copied through unescaped
 * rather than being split into one \u00XX escape per byte. Splitting them up
 * looks like a safe, parser-agnostic choice, but it isn't: a JSON decoder
 * turns á into the single codepoint U+00E1, not the raw byte 0xE1, so a
 * multi-byte UTF-8 character escaped this way decodes into a run of
 * Latin-1-valued codepoints instead of the original character -- valid JSON,
 * but mojibake for any UTF-8-aware reader (a terminal, or Python's
 * str(json.loads(...))). Emitting the bytes raw keeps the JSON string
 * correct UTF-8, matching how src/sync/DownloadQueue.cpp's logStage already
 * embeds book paths via plain %s.
 */
void appendJsonEscaped(std::string& out, const char* data, size_t len);

/**
 * Renders a unix epoch as an ISO calendar date, "YYYY-MM-DD".
 *
 * This device has no clock (no RTC on most boards, and sleep is a full chip
 * reset that would lose one anyway), so it can never say what day it is --
 * but a timestamp that arrived from the server is a fact in itself, and
 * showing "the other device was here on 2026-08-24" needs no local clock at
 * all. Numeric on purpose: month names would need twelve new translations
 * for one line of prose.
 *
 * `utcOffsetSeconds` shifts the instant before the date is taken, so the day
 * boundary matches the reader's own. Callers pass
 * (SETTINGS.clockUtcOffsetQ - 48) * 900 -- the biased quarter-hour offset
 * HalClock::formatTime already takes.
 */
std::string formatUtcDate(int64_t epochSeconds, int32_t utcOffsetSeconds);

}  // namespace StringUtils
