#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Pure decision for "what goes in the three recent-book slots on Home?" --
// factored out so HomeActivity's render code never has to reason about
// remote-vs-local precedence, queue state, or dedup itself (see
// test/home_book_slots). The device-only caller builds Input from the
// manifest, DownloadQueue, and the SD-backed recents list, none of which
// this file includes -- see HomeActivity.cpp for how each field below is
// obtained once that wiring lands.
//
// The constraints this encodes, from the task brief:
//  - Remote books (things the server has that this device does not) always
//    outrank recents, because a book waiting to be read is more actionable
//    than one already finished or in progress.
//  - The cover tile band above the slots shows its own books already, so
//    fill() must never repeat them: coverTilePaths exists purely to be
//    subtracted out. It is a list, not one path, because the band is as
//    wide as the theme's homeRecentBooksCount -- three on Lyra3Covers.
//  - A recent that has become a remote candidate again (re-uploaded, or the
//    same path reappearing in the manifest) is deduplicated against
//    whichever remote slot already claimed it, rather than shown twice.
//  - A remote book's title and author are derived from its path, not read
//    from the manifest (which has neither field) or the EPUB (which does
//    not exist on this device yet). Putting the containing folder where the
//    author goes is a deliberate product decision, not a placeholder: it
//    keeps the row's three lines meaningful, and the folder is the one
//    useful thing the manifest does know about a book nobody here has
//    opened.
namespace home_book_slots {

constexpr size_t SLOT_COUNT = 3;

enum class State { OnServer, Queued, Downloading, Failed, JustDownloaded, Read };

// A book the manifest says the server has, already filtered by the caller
// to `downloaded == false` -- this file has no concept of what is already
// on SD, so that filter is the caller's job, not fill()'s.
struct RemoteCandidate {
  std::string id;
  std::string path;
  uint64_t sizeBytes = 0;
  // Opaque server-assigned ordering value. Compared only -- see the
  // implementation note on why it is never subtracted or formatted.
  uint64_t updatedAt = 0;
};

// A book already on SD, in whatever order the caller's recents list keeps
// them (most-recently-opened first, in practice, but fill() does not
// assume that -- it preserves whatever order it is given).
struct RecentCandidate {
  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;
  int progressPercent = -1;
};

// A book that finished downloading in this session and has not been opened
// yet. It is in neither of the other two sources: markDownloaded() drops it
// from the manifest scan the moment the transfer ends, and a book only
// enters recents once the reader opens it. sizeBytes is carried when the
// caller has it cheaply to hand and left 0 otherwise, in which case the row
// omits the size rather than printing a zero.
struct DownloadedCandidate {
  std::string path;
  uint64_t sizeBytes = 0;
};

// What the queue says right now, flattened so this stays free of
// DownloadQueue -- fill() only needs to know, per remote id, whether it is
// queued (and at what position) or was the most recent failure.
struct QueueView {
  struct Entry {
    std::string id;
    State state = State::Queued;
    int position = 0;
  };
  std::vector<Entry> entries;
  // Empty when the last finish() succeeded, or nothing has finished yet.
  std::string lastFailedId;
};

struct Slot {
  bool remote = false;       // true: a manifest book; false: a local recent
  std::string id;             // manifest id, empty for a recent
  std::string path;
  // For a remote book, both are derived from path (see fill()'s
  // implementation): the manifest carries no title or author, since those
  // come from opening the EPUB, which does not exist on this device yet.
  std::string title;
  std::string author;
  std::string coverBmpPath;   // empty for a remote book -- it has no cover
  int progressPercent = -1;
  uint64_t sizeBytes = 0;      // from the candidate for a remote book; unused (0) for a recent
  State state = State::OnServer;
  int queuePosition = 0;       // 1-based, only meaningful when state == Queued
};

struct Input {
  std::vector<RemoteCandidate> remote;   // already filtered to downloaded == false
  // Books that finished downloading in THIS session and have not been opened
  // yet. Keyed by path, not id: once a download lands the book is a local
  // file, and every other source here knows it only by path. Without this
  // source such a book would disappear from Home at the exact moment the
  // reader finished waiting for it.
  std::vector<DownloadedCandidate> justDownloaded;
  std::vector<RecentCandidate> recents;
  QueueView queue;
  // The books already shown in the cover tile band, never repeated below.
  // One per tile the active theme draws, not just the first: with a single
  // path, a three-tile theme showed its second and third books twice.
  std::vector<std::string> coverTilePaths;
};

// Rules, in order:
//  1. Remote books first, sorted by updatedAt descending, ties broken by
//     path ascending so the result is deterministic.
//  2. Then justDownloaded in its given order.
//  3. Then recents in their given order.
//  4. Every source after the first skips a candidate whose path is in
//     coverTilePaths, or matches one already placed. A justDownloaded book
//     is additionally skipped once it appears in recents: being there means
//     the reader has opened it, so it retires to an ordinary Read row rather
//     than wearing the NEW badge for the rest of the session.
//  5. Stop at SLOT_COUNT. Fewer than three inputs yields fewer than three
//     slots, and three remote books push everything else out entirely:
//     remote outranks the rest and no source has a reserved slot.
//  6. State for a remote book: Downloading or Queued if the queue lists its
//     id (carry position); else Failed if its id equals queue.lastFailedId;
//     else OnServer. A justDownloaded book that survives rule 4 is always
//     JustDownloaded, and a recent is always Read.
//  7. A remote or justDownloaded slot's title is its path's filename without
//     directory or extension, and its author is the containing folder's
//     name, or empty at the root -- neither has ever been opened here, so
//     there is no parsed title and no cover. sizeBytes is copied straight
//     from the candidate. A recent slot keeps the
//     title/author/coverBmpPath/progressPercent it was given and leaves
//     sizeBytes at 0.
std::vector<Slot> fill(const Input& in);

}  // namespace home_book_slots
