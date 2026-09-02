#include "HomeBookSlots.h"

#include <algorithm>

namespace home_book_slots {

namespace {

// Looks up a remote id in the queue view, returning the entry it finds so
// the caller can read both state and position in one pass instead of
// scanning entries twice.
const QueueView::Entry* findQueueEntry(const QueueView& queue, const std::string& id) {
  for (const QueueView::Entry& entry : queue.entries) {
    if (entry.id == id) return &entry;
  }
  return nullptr;
}

// Whether any slot already placed has this path -- a linear scan over at
// most SLOT_COUNT entries, which is cheaper and allocation-free next to the
// hash-table overhead an unordered_set would carry for the same three
// strings.
bool alreadyPlaced(const std::vector<Slot>& slots, const std::string& path) {
  for (const Slot& slot : slots) {
    if (slot.path == path) return true;
  }
  return false;
}

// Whether path is one of the books the cover tile band already shows. A
// linear scan over at most a theme's homeRecentBooksCount entries (three, on
// the widest theme).
bool isCoverTile(const Input& in, const std::string& path) {
  for (const std::string& tile : in.coverTilePaths) {
    if (tile == path) return true;
  }
  return false;
}

// Whether the reader has opened this book, which is the only thing that
// retires its NEW badge -- see rule 4 in HomeBookSlots.h.
bool isInRecents(const Input& in, const std::string& path) {
  for (const RecentCandidate& candidate : in.recents) {
    if (candidate.path == path) return true;
  }
  return false;
}

// The filename component of path, i.e. everything after the last '/' (the
// whole string if there is none).
std::string filenameOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// A remote book's title, since the manifest carries no title field: the
// filename with its directory and extension stripped.
std::string titleFromPath(const std::string& path) {
  const std::string filename = filenameOf(path);
  const size_t dot = filename.find_last_of('.');
  return dot == std::string::npos ? filename : filename.substr(0, dot);
}

// A remote book's author, since the manifest carries no author field: the
// name of the folder directly containing it, or empty at the root. See
// HomeBookSlots.h for why the folder goes here deliberately.
std::string authorFromPath(const std::string& path) {
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash == std::string::npos || lastSlash == 0) return "";
  const size_t secondLastSlash = path.find_last_of('/', lastSlash - 1);
  const size_t start = secondLastSlash == std::string::npos ? 0 : secondLastSlash + 1;
  return path.substr(start, lastSlash - start);
}

State stateForRemote(const RemoteCandidate& candidate, const QueueView& queue) {
  if (const QueueView::Entry* entry = findQueueEntry(queue, candidate.id)) {
    return entry->state;
  }
  if (!queue.lastFailedId.empty() && queue.lastFailedId == candidate.id) {
    return State::Failed;
  }
  return State::OnServer;
}

int queuePositionForRemote(const RemoteCandidate& candidate, const QueueView& queue) {
  if (const QueueView::Entry* entry = findQueueEntry(queue, candidate.id)) {
    return entry->position;
  }
  return 0;
}

}  // namespace

std::vector<Slot> fill(const Input& in) {
  std::vector<RemoteCandidate> sortedRemote = in.remote;
  // updatedAt is opaque and only ever compared, never interpreted as a
  // date -- see HomeBookSlots.h. Descending so the most recently updated
  // book leads; path ascending as the tiebreak keeps the result
  // deterministic when two rows tie.
  std::sort(sortedRemote.begin(), sortedRemote.end(), [](const RemoteCandidate& a, const RemoteCandidate& b) {
    if (a.updatedAt != b.updatedAt) return a.updatedAt > b.updatedAt;
    return a.path < b.path;
  });

  std::vector<Slot> slots;
  slots.reserve(SLOT_COUNT);

  for (const RemoteCandidate& candidate : sortedRemote) {
    if (slots.size() >= SLOT_COUNT) break;

    Slot slot;
    slot.remote = true;
    slot.id = candidate.id;
    slot.path = candidate.path;
    slot.title = titleFromPath(candidate.path);
    slot.author = authorFromPath(candidate.path);
    slot.sizeBytes = candidate.sizeBytes;
    slot.state = stateForRemote(candidate, in.queue);
    slot.queuePosition = queuePositionForRemote(candidate, in.queue);
    slots.push_back(std::move(slot));
  }

  // Between the two: the manifest scan has already dropped these (the
  // markDownloaded() write lands as the transfer ends) and recents will not
  // hold them until the reader opens one, so this is the only source that
  // keeps a book visible in the moment it stops being a download.
  for (const DownloadedCandidate& candidate : in.justDownloaded) {
    if (slots.size() >= SLOT_COUNT) break;
    if (isCoverTile(in, candidate.path)) continue;
    if (alreadyPlaced(slots, candidate.path)) continue;
    // Opened since it landed: the badge has done its job, so fall through and
    // let the recents loop below place it as an ordinary Read row.
    if (isInRecents(in, candidate.path)) continue;

    Slot slot;
    slot.remote = false;
    slot.path = candidate.path;
    // Local, but never opened here: no parsed title, no cover. Same
    // derivation the remote branch above uses.
    slot.title = titleFromPath(candidate.path);
    slot.author = authorFromPath(candidate.path);
    slot.sizeBytes = candidate.sizeBytes;
    slot.state = State::JustDownloaded;
    slots.push_back(std::move(slot));
  }

  for (const RecentCandidate& candidate : in.recents) {
    if (slots.size() >= SLOT_COUNT) break;
    if (isCoverTile(in, candidate.path)) continue;
    if (alreadyPlaced(slots, candidate.path)) continue;

    Slot slot;
    slot.remote = false;
    slot.path = candidate.path;
    slot.title = candidate.title;
    slot.author = candidate.author;
    slot.coverBmpPath = candidate.coverBmpPath;
    slot.progressPercent = candidate.progressPercent;
    slot.state = State::Read;
    slots.push_back(std::move(slot));
  }

  return slots;
}

}  // namespace home_book_slots
