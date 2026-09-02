#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Pure decision for "what goes in the three recent-book slots on Home?" --
// factored out so HomeActivity's render code never has to reason about queue
// state or the cover tile itself (see test/home_book_slots).
//
// There is one source: the recency list (RecentBooksStore), already ordered
// most-recent-first and already holding both kinds of entry -- books on the
// SD card, and books the server has that this device has not downloaded. See
// docs/superpowers/specs/2026-09-02-one-recency-list-design.md. Home takes
// the first SLOT_COUNT entries, skipping whatever the cover tile band above
// already shows. That is the whole ordering rule: there is nothing here to
// merge, sort or deduplicate, because the list itself is the sequence.
//
// coverTilePaths is a list, not one path, because the band is as wide as the
// theme's homeRecentBooksCount -- three on Lyra3Covers, and with a single
// path its second and third books appeared again as rows.
//
// A remote entry's title and author are derived from its path, not read from
// the manifest (which has neither field) or the EPUB (which does not exist on
// this device yet). Putting the containing folder where the author goes is a
// deliberate product decision, not a placeholder: it keeps the row's three
// lines meaningful, and the folder is the one useful thing known about a book
// nobody here has opened.
namespace home_book_slots {

constexpr size_t SLOT_COUNT = 3;

enum class State { OnServer, Queued, Downloading, Failed, JustDownloaded, Read };

// One entry of the recency list, in the order the list keeps it (fill() does
// not sort -- it preserves whatever order it is given).
struct RecentCandidate {
  std::string path;
  // All four are empty/unset for a remote entry: nothing on this device has
  // ever opened that book, so there is no parsed metadata and no cover.
  std::string title;
  std::string author;
  std::string coverBmpPath;
  int progressPercent = -1;
  // Manifest id when the server has this book and this device does not.
  // Empty for a book on the SD card.
  std::string remoteId;
  // Server-reported size, for a remote entry only; 0 when unknown.
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
  bool remote = false;  // true: the server has it and this device does not
  std::string id;       // manifest id, empty for a local entry
  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;  // empty for a remote entry -- it has no cover
  int progressPercent = -1;
  uint64_t sizeBytes = 0;  // from the entry for a remote book; 0 for a local one
  State state = State::OnServer;
  int queuePosition = 0;  // 1-based, only meaningful when state == Queued
};

struct Input {
  // The recency list, most recent first, local and remote entries together.
  std::vector<RecentCandidate> recents;
  QueueView queue;
  // The books already shown in the cover tile band, never repeated below.
  // One per tile the active theme draws, not just the first.
  std::vector<std::string> coverTilePaths;
};

// Rules, in order:
//  1. Walk recents in the order given, skipping any entry whose path is in
//     coverTilePaths, until SLOT_COUNT slots are filled or the list runs out.
//  2. State for a remote entry (non-empty remoteId): Downloading or Queued if
//     the queue lists its id (carry position); else Failed if its id equals
//     queue.lastFailedId; else OnServer.
//  3. State for a local entry: Read once it has a cached progressPercent, and
//     JustDownloaded until then. Every close of a book funnels through
//     RecentBooksStore::updateProgressPercent(), so "has no percentage yet"
//     is exactly "has never been opened on this device" -- which is what the
//     NEW badge means. Nothing else survives a sleep to say it: the board has
//     no clock, and sleep is a full chip reset.
//  4. A remote entry's title is its path's filename without directory or
//     extension, its author is the containing folder's name (empty at the
//     root), and its sizeBytes comes from the entry. A local entry keeps the
//     title/author/coverBmpPath/progressPercent it was given and leaves
//     sizeBytes at 0.
std::vector<Slot> fill(const Input& in);

// Rule 4's title/author derivation, exposed on their own: RecentBooksActivity
// shows the same recency list as Home (see
// docs/superpowers/specs/2026-09-02-one-recency-list-design.md's "Recent
// Books screen") and needs the identical title/author for a remote entry
// without duplicating the split logic here.
std::string titleFromPath(const std::string& path);
std::string authorFromPath(const std::string& path);

// Rule 2's queue lookup, exposed the same way: Downloading/Queued (with
// position) if the queue lists remoteId, else Failed if it was the queue's
// last failure, else OnServer. Writes queuePosition (0 unless the returned
// state is Queued). Same precedence Home's rows use -- the queue outranks a
// stale failure, so a retry reads as Queued rather than resurfacing the old
// error.
State remoteState(const std::string& remoteId, const QueueView& queue, int& queuePosition);

}  // namespace home_book_slots
