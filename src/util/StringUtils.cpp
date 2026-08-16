#include "StringUtils.h"

#include <Utf8.h>

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

}  // namespace StringUtils
