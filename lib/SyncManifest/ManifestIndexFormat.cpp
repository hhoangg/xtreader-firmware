#include "ManifestIndexFormat.h"

#include <cstdlib>

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
  if (fieldIdx != FIELD_COUNT) return false;  // too few fields -- corrupt line
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
