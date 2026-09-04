#pragma once

#include <cstdint>
#include <string>

// Pure decision for "the server has a position for this book -- do we
// interrupt the reader about it?" -- factored out of EpubReaderActivity so
// the rule can be host-tested without ESP-IDF/Arduino (see
// test/remote_progress_policy). The device-only caller is
// EpubReaderActivity::loop(), which is where the inputs below are actually
// obtained; see src/sync/RemoteProgressCheck.h for how the remote half of
// them arrives.
//
// The constraints this encodes:
//  - Progress this device uploaded itself is never worth a prompt. That is
//    the only reason selfDeviceId exists.
//  - The gate is the chapter, never the percentage. The two sides do not
//    measure the same thing: KOReader's default page mode returns
//    current_page / number_of_pages, which moves with font, margins, line
//    spacing and embedded CSS, while this device derives a byte fraction
//    from Epub::calculateProgress. Comparing them is a systematic
//    multi-percent offset, not noise -- it made every later open of an
//    already-resolved book ask again. The xpath's DocFragment index is
//    accurate in both of KOReader's view modes, and reading it costs no
//    chapter decompression, so it is what decides.
//  - An unreadable chapter biases toward asking, not toward silence. A
//    missed prompt strands the reader at the wrong place with no clue why;
//    an extra prompt is one button press.
//  - This device has no clock, so "which position is newer" is not
//    answerable here. What is answerable is "have we already been asked
//    about this exact row", because the server stamps the row -- hence
//    resolvedTimestamp, which is stored and compared, never interpreted.
namespace remote_progress_policy {

// Every CrossPoint reader sent this literal as its `device_id` before
// per-device ids went on the wire (KOReaderSyncClient.cpp's DEVICE_ID
// constant). Rows written then are indistinguishable from another
// CrossPoint device's -- treating them as our own is the conservative
// reading, and it is what lets the change ship without a server-side
// backfill.
constexpr char LEGACY_DEVICE_ID[] = "crosspoint-reader";

// No chapter could be read out of the remote row's `progress` string.
// Chapters are 1-based, matching KOReader's DocFragment index, so zero is
// free to mean "unknown".
constexpr int UNKNOWN_CHAPTER = 0;

// Chapter named by a KOReader `progress` string, 1-based, or UNKNOWN_CHAPTER.
// Subtract one to compare it against a spine index.
//
// The shapes this has to survive, measured across the .sdr files of one
// ordinary Kindle:
//   /body/DocFragment[N]/...   EPUB, the expected shape          -> N
//   /body/DocFragment/...      single-fragment EPUB, no index    -> 1
//   /FictionBook/body/...      FB2, no DocFragment at all        -> unknown
//   /html/body/..., /html[2]/body/...   HTML/TXT                 -> unknown
//   "173"                      PDF/DJVU page number, not a path  -> unknown
// Only the first two are reachable with the same document hash as an EPUB,
// but a wrong guess here is a silent misfire, so the rest are rejected
// explicitly rather than left to fall out of the parse.
int chapterFromProgress(const std::string& progress);

struct Input {
  // False when the fetch failed, was skipped, or the server had no row --
  // RemoteProgressCheck collapses all of those into one "nothing to say".
  bool haveRemote = false;
  // `device_id` as the server returned it. Empty means the client that wrote
  // the row never sent one.
  std::string remoteDeviceId;
  // This device's own id, from SYNC_STORE.getDeviceId(). Empty on an
  // unpaired device, which cannot reach this code anyway.
  std::string selfDeviceId;
  // The row's `progress` field verbatim -- a KOReader xpointer for a
  // reflowable format, a bare page number for a paged one.
  std::string remoteProgress;
  // Where this device currently is, as a spine index. 0-based, where
  // DocFragment is 1-based.
  int localSpineIndex = 0;
  // The server's `timestamp` for this row, and the newest one this document
  // has already been answered about (RemoteProgressMarker, 0 when never).
  // A row at or below the marker has been resolved once already; a
  // genuinely newer position from the other device is above it and still
  // prompts.
  int64_t remoteTimestamp = 0;
  int64_t resolvedTimestamp = 0;
};

enum class Decision { Ignore, Prompt };

Decision decide(const Input& in);

}  // namespace remote_progress_policy
