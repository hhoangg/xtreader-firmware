#include "RemoteProgressPolicy.h"

#include <cstddef>

namespace remote_progress_policy {

namespace {

bool isThisDevice(const std::string& remoteDeviceId, const std::string& selfDeviceId) {
  if (remoteDeviceId.empty()) return false;
  if (remoteDeviceId == LEGACY_DEVICE_ID) return true;
  return !selfDeviceId.empty() && remoteDeviceId == selfDeviceId;
}

constexpr char DOC_FRAGMENT[] = "/body/DocFragment";
constexpr std::size_t DOC_FRAGMENT_LEN = sizeof(DOC_FRAGMENT) - 1;

// A spine longer than this is not a book, it is a malformed index. Bounding
// the digits also keeps the accumulator from overflowing on a garbage row.
constexpr int MAX_CHAPTER = 99999;

}  // namespace

int chapterFromProgress(const std::string& progress) {
  if (progress.compare(0, DOC_FRAGMENT_LEN, DOC_FRAGMENT) != 0) return UNKNOWN_CHAPTER;

  std::size_t pos = DOC_FRAGMENT_LEN;
  // A single-fragment EPUB drops the index entirely: "/body/DocFragment/body/...".
  // The '.' case is the same document with the terminal offset glued straight on.
  if (pos == progress.size()) return 1;
  const char next = progress[pos];
  if (next == '/' || next == '.') return 1;
  if (next != '[') return UNKNOWN_CHAPTER;

  ++pos;
  int chapter = 0;
  std::size_t digits = 0;
  while (pos < progress.size() && progress[pos] >= '0' && progress[pos] <= '9') {
    chapter = chapter * 10 + (progress[pos] - '0');
    if (chapter > MAX_CHAPTER) return UNKNOWN_CHAPTER;
    ++pos;
    ++digits;
  }
  if (digits == 0) return UNKNOWN_CHAPTER;
  if (pos >= progress.size() || progress[pos] != ']') return UNKNOWN_CHAPTER;
  // DocFragment is 1-based, so [0] is not a chapter this device can map.
  if (chapter < 1) return UNKNOWN_CHAPTER;
  return chapter;
}

Decision decide(const Input& in) {
  if (!in.haveRemote) return Decision::Ignore;
  if (isThisDevice(in.remoteDeviceId, in.selfDeviceId)) return Decision::Ignore;

  // Already answered, whichever way the reader answered it. An unstamped row
  // has no key to compare, so it falls through to the chapter gate rather
  // than being silently swallowed.
  if (in.remoteTimestamp > 0 && in.remoteTimestamp <= in.resolvedTimestamp) return Decision::Ignore;

  const int chapter = chapterFromProgress(in.remoteProgress);
  if (chapter == UNKNOWN_CHAPTER) return Decision::Prompt;
  if (chapter - 1 == in.localSpineIndex) return Decision::Ignore;
  return Decision::Prompt;
}

}  // namespace remote_progress_policy
