#include "HomeBookSlots.h"

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

// Whether path is one of the books the cover tile band already shows. A
// linear scan over at most a theme's homeRecentBooksCount entries (three, on
// the widest theme).
bool isCoverTile(const Input& in, const std::string& path) {
  for (const std::string& tile : in.coverTilePaths) {
    if (tile == path) return true;
  }
  return false;
}

// The filename component of path, i.e. everything after the last '/' (the
// whole string if there is none).
std::string filenameOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Fills in everything a remote entry's row shows.
void applyRemote(Slot& slot, const RecentCandidate& candidate, const QueueView& queue) {
  slot.remote = true;
  slot.id = candidate.remoteId;
  slot.title = titleFromPath(candidate.path);
  slot.author = authorFromPath(candidate.path);
  slot.sizeBytes = candidate.sizeBytes;
  slot.state = remoteState(candidate.remoteId, queue, slot.queuePosition);
}

void applyLocal(Slot& slot, const RecentCandidate& candidate) {
  slot.title = candidate.title;
  slot.author = candidate.author;
  slot.coverBmpPath = candidate.coverBmpPath;
  slot.progressPercent = candidate.progressPercent;
  // No cached percentage means the reader has never closed this book here --
  // see rule 3 in HomeBookSlots.h.
  slot.state = candidate.progressPercent < 0 ? State::JustDownloaded : State::Read;
}

}  // namespace

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

State remoteState(const std::string& remoteId, const QueueView& queue, int& queuePosition) {
  queuePosition = 0;
  if (const QueueView::Entry* entry = findQueueEntry(queue, remoteId)) {
    queuePosition = entry->position;
    return entry->state;
  }
  return (!queue.lastFailedId.empty() && queue.lastFailedId == remoteId) ? State::Failed : State::OnServer;
}

std::vector<Slot> fill(const Input& in) {
  std::vector<Slot> slots;
  slots.reserve(SLOT_COUNT);

  for (const RecentCandidate& candidate : in.recents) {
    if (slots.size() >= SLOT_COUNT) break;
    if (isCoverTile(in, candidate.path)) continue;

    Slot slot;
    slot.path = candidate.path;
    if (candidate.remoteId.empty()) {
      applyLocal(slot, candidate);
    } else {
      applyRemote(slot, candidate, in.queue);
    }
    slots.push_back(std::move(slot));
  }

  return slots;
}

}  // namespace home_book_slots
