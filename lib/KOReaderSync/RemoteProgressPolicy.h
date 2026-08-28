#pragma once

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
//  - Silence is the default. Every uncertain case resolves to Ignore
//    except an unidentified remote row (see below), because a dialog the
//    reader did not need is worse than a jump they can still make by hand
//    from the reader menu.
//  - Progress this device uploaded itself is never worth a prompt. That is
//    the only reason selfDeviceId exists.
//  - This device has no clock, and the local position carries no timestamp
//    (progress.bin holds spine/page/pageCount/offset and nothing else), so
//    "which one is newer" is not answerable here. Position is what gets
//    compared, and the reader arbitrates.
namespace remote_progress_policy {

// Every CrossPoint reader sent this literal as its `device_id` before
// per-device ids went on the wire (KOReaderSyncClient.cpp's DEVICE_ID
// constant). Rows written then are indistinguishable from another
// CrossPoint device's -- treating them as our own is the conservative
// reading, and it is what lets the change ship without a server-side
// backfill.
constexpr char LEGACY_DEVICE_ID[] = "crosspoint-reader";

// How far apart two positions must be before the difference is real.
// One percent is roughly two or three pages in a 300-page book. Both sides
// derive percentage from byte offsets independently -- KOReader from its own
// document model, this device from Epub::calculateProgress -- so a tighter
// threshold prompts on rounding rather than on a reader who actually moved.
constexpr float PROMPT_THRESHOLD = 0.01f;

struct Input {
  // False when the fetch failed, was skipped, or the server had no row --
  // RemoteProgressCheck collapses all of those into one "nothing to say".
  bool haveRemote = false;
  // Server's percentage for this document, 0..1.
  float remotePercentage = 0.0f;
  // `device_id` as the server returned it. Empty means the client that wrote
  // the row never sent one.
  std::string remoteDeviceId;
  // This device's own id, from SYNC_STORE.getDeviceId(). Empty on an
  // unpaired device, which cannot reach this code anyway.
  std::string selfDeviceId;
  // Where this device currently is in the book, 0..1, from
  // Epub::calculateProgress.
  float localPercentage = 0.0f;
};

enum class Decision { Ignore, Prompt };

Decision decide(const Input& in);

}  // namespace remote_progress_policy
