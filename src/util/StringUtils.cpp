#include "StringUtils.h"

#include <Utf8.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace StringUtils {

std::string sanitizeFilename(const std::string& name, size_t maxBytes) {
  std::string result;
  result.reserve(std::min(name.size(), maxBytes));

  const auto* text = reinterpret_cast<const unsigned char*>(name.c_str());

  // Skip leading spaces and dots so they don't consume the byte budget
  while (*text == ' ' || *text == '.') {
    text++;
  }

  // Process full UTF-8 codepoints to avoid trimming in the middle of a multibyte sequence
  while (*text != 0) {
    const auto* cpStart = text;
    uint32_t cp = utf8NextCodepoint(&text);

    if (cp == '/' || cp == '\\' || cp == ':' || cp == '*' || cp == '?' || cp == '"' || cp == '<' || cp == '>' ||
        cp == '|') {
      // Replace illegal and control characters with '_'
      if (result.length() + 1 > maxBytes) break;
      result += '_';
    } else if (cp >= 128 || (cp >= 32 && cp < 127)) {
      const size_t cpBytes = text - cpStart;
      if (result.length() + cpBytes > maxBytes) break;
      result.append(reinterpret_cast<const char*>(cpStart), cpBytes);
    }
  }

  // Trim trailing spaces and dots
  size_t end = result.find_last_not_of(" .");
  if (end != std::string::npos) {
    result.resize(end + 1);
  } else {
    result.clear();
  }

  return result.empty() ? "book" : result;
}

std::string middleEllipsis(const std::string& value, size_t maxChars) {
  constexpr char ELLIPSIS[] = "...";
  constexpr size_t ELLIPSIS_CHARS = 3;

  // Record each codepoint's starting byte offset, plus a trailing sentinel
  // for the end of the string, so the head/tail slices below land on UTF-8
  // boundaries -- this is written for URLs (ASCII in practice), but a
  // self-hoster can type anything into the custom server URL field. The
  // sentinel (offsets[totalChars] == value.size()) is what lets
  // offsets[totalChars - tailChars] below stay in bounds even when
  // tailChars is 0 (an empty tail).
  std::vector<size_t> offsets;
  offsets.reserve(value.size() + 1);
  const auto* p = reinterpret_cast<const unsigned char*>(value.data());
  const unsigned char* end = p + value.size();
  while (p < end) {
    offsets.push_back(static_cast<size_t>(reinterpret_cast<const char*>(p) - value.data()));
    utf8NextCodepoint(&p);
  }
  const size_t totalChars = offsets.size();
  offsets.push_back(value.size());

  if (totalChars <= maxChars) return value;

  if (maxChars <= ELLIPSIS_CHARS) {
    // Not enough room for any head/tail context -- just however much of the
    // marker itself fits.
    return std::string(ELLIPSIS).substr(0, maxChars);
  }

  // Split what's left between head and tail, favoring the head by one
  // character when the budget is odd -- a URL's scheme/host at the front is
  // usually the more useful half to keep intact.
  const size_t budget = maxChars - ELLIPSIS_CHARS;
  const size_t headChars = (budget + 1) / 2;
  const size_t tailChars = budget - headChars;

  const size_t headEndByte = offsets[headChars];
  const size_t tailStartByte = offsets[totalChars - tailChars];

  return value.substr(0, headEndByte) + ELLIPSIS + value.substr(tailStartByte);
}

void appendJsonEscaped(std::string& out, const char* data, size_t len) {
  out += '"';
  for (size_t i = 0; i < len; i++) {
    const uint8_t c = static_cast<uint8_t>(data[i]);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20 || c == 0x7f) {
          char esc[7];
          snprintf(esc, sizeof(esc), "\\u%04x", c);
          out += esc;
        } else {
          out += static_cast<char>(c);
        }
        break;
    }
  }
  out += '"';
}

std::string formatUtcDate(const int64_t epochSeconds, const int32_t utcOffsetSeconds) {
  const int64_t local = epochSeconds + utcOffsetSeconds;
  // Floor division: C++'s / truncates toward zero, which would put every
  // instant before 1970 on the wrong day.
  int64_t days = local / 86400;
  if (local % 86400 < 0) days--;

  // Howard Hinnant's civil_from_days: shift the era so the leap-day
  // irregularity lands at the end of the cycle, then walk down era ->
  // year-of-era -> day-of-year -> month. No <ctime>, no timezone database,
  // no allocation.
  days += 719468;  // shift epoch from 1970-01-01 to 0000-03-01
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t dayOfEra = days - era * 146097;                                                         // [0, 146096]
  const int64_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;  // [0, 399]
  const int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);             // [0, 365]
  const int64_t mp = (5 * dayOfYear + 2) / 153;            // [0, 11], March-based
  const int64_t day = dayOfYear - (153 * mp + 2) / 5 + 1;  // [1, 31]
  const int64_t month = mp < 10 ? mp + 3 : mp - 9;         // [1, 12]
  const int64_t year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);

  char buf[16];
  snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lld", (long long)year, (long long)month, (long long)day);
  return std::string(buf);
}

}  // namespace StringUtils
