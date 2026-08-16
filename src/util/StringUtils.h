#pragma once

#include <cctype>
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

}  // namespace StringUtils
