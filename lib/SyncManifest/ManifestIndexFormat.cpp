#include "ManifestIndexFormat.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr char DELIM = '|';
constexpr int FIELD_COUNT = 6;  // id, path, sizeBytes, contentHash, updatedAt, downloaded

// strtoull, not the JSON-side parseUintToken (StreamingJsonParser.h stops at
// the first non-digit without flagging it): this format wants a strict
// "the whole field is digits" check, since a partially-numeric field here
// means the index line is corrupt, not that trailing junk should be ignored.
uint64_t parseStrictUint64(const std::string& s, bool& ok) {
  if (s.empty()) {
    ok = false;
    return 0;
  }
  char* end = nullptr;
  const unsigned long long v = strtoull(s.c_str(), &end, 10);
  ok = end != nullptr && *end == '\0';
  return static_cast<uint64_t>(v);
}
}  // namespace

std::string formatIndexLine(const ManifestIndexRecord& record) {
  std::string line;
  line.reserve(record.id.size() + record.path.size() + record.contentHash.size() + 32);
  line += record.id;
  line += DELIM;
  line += record.path;
  line += DELIM;
  line += std::to_string(record.sizeBytes);
  line += DELIM;
  line += record.contentHash;
  line += DELIM;
  line += std::to_string(record.updatedAt);
  line += DELIM;
  line += record.downloaded ? '1' : '0';
  line += '\n';
  return line;
}

bool parseIndexLine(const char* line, size_t len, ManifestIndexRecord& out) {
  out = ManifestIndexRecord{};
  if (len == 0) return false;

  std::string fields[FIELD_COUNT];
  int fieldIdx = 0;
  size_t fieldStart = 0;
  for (size_t i = 0; i <= len; ++i) {
    const bool atDelim = i < len && line[i] == DELIM;
    const bool atEnd = i == len;
    if (!atDelim && !atEnd) continue;
    if (fieldIdx >= FIELD_COUNT) return false;  // too many fields -- corrupt line
    fields[fieldIdx++].assign(line + fieldStart, i - fieldStart);
    fieldStart = i + 1;
  }
  if (fieldIdx != FIELD_COUNT) return false;                 // too few fields -- corrupt line
  if (fields[0].empty() || fields[1].empty()) return false;  // id/path are required

  bool ok = true;
  out.id = fields[0];
  out.path = fields[1];
  out.sizeBytes = parseStrictUint64(fields[2], ok);
  if (!ok) return false;
  out.contentHash = fields[3];
  out.updatedAt = parseStrictUint64(fields[4], ok);
  if (!ok) return false;
  if (fields[5] != "0" && fields[5] != "1") return false;
  out.downloaded = fields[5] == "1";
  return true;
}

std::string formatIndexHeader(const uint32_t version, const uint64_t watermark) {
  char buf[INDEX_HEADER_LEN + 1];  // +1 for snprintf's trailing NUL, not part of the returned bytes
  const int n =
      snprintf(buf, sizeof(buf), "%s|%0*u|%0*llu\n", INDEX_HEADER_MAGIC, static_cast<int>(INDEX_HEADER_VERSION_DIGITS),
               version, static_cast<int>(INDEX_HEADER_WATERMARK_DIGITS), static_cast<unsigned long long>(watermark));
  // n is the length snprintf *would* have written; a version/watermark that overflows its field
  // width (impossible for a uint32_t/uint64_t in 10/20 digits respectively) would make this a
  // programmer error worth a loud fixed-width mismatch rather than a silently truncated header.
  return std::string(buf, n > 0 && static_cast<size_t>(n) == INDEX_HEADER_LEN ? INDEX_HEADER_LEN : 0);
}

bool parseIndexHeader(const char* data, const size_t len, uint32_t& version, uint64_t& watermark) {
  if (len != INDEX_HEADER_LEN) return false;
  if (memcmp(data, INDEX_HEADER_MAGIC, INDEX_HEADER_MAGIC_LEN) != 0) return false;

  constexpr size_t versionStart = INDEX_HEADER_MAGIC_LEN + 1;
  constexpr size_t versionEnd = versionStart + INDEX_HEADER_VERSION_DIGITS;
  constexpr size_t watermarkStart = versionEnd + 1;
  constexpr size_t watermarkEnd = watermarkStart + INDEX_HEADER_WATERMARK_DIGITS;
  static_assert(watermarkEnd + 1 == INDEX_HEADER_LEN, "header field layout must exactly fill INDEX_HEADER_LEN");

  if (data[INDEX_HEADER_MAGIC_LEN] != '|' || data[versionEnd] != '|' || data[watermarkEnd] != '\n') return false;
  for (size_t i = versionStart; i < versionEnd; i++) {
    if (!isdigit(static_cast<unsigned char>(data[i]))) return false;
  }
  for (size_t i = watermarkStart; i < watermarkEnd; i++) {
    if (!isdigit(static_cast<unsigned char>(data[i]))) return false;
  }

  char versionBuf[INDEX_HEADER_VERSION_DIGITS + 1];
  memcpy(versionBuf, data + versionStart, INDEX_HEADER_VERSION_DIGITS);
  versionBuf[INDEX_HEADER_VERSION_DIGITS] = '\0';
  char watermarkBuf[INDEX_HEADER_WATERMARK_DIGITS + 1];
  memcpy(watermarkBuf, data + watermarkStart, INDEX_HEADER_WATERMARK_DIGITS);
  watermarkBuf[INDEX_HEADER_WATERMARK_DIGITS] = '\0';

  version = static_cast<uint32_t>(strtoul(versionBuf, nullptr, 10));
  watermark = static_cast<uint64_t>(strtoull(watermarkBuf, nullptr, 10));
  return true;
}
