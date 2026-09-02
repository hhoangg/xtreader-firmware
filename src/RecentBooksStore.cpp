#include "RecentBooksStore.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <RecentDiscovery.h>
#include <Xtc.h>

#include <algorithm>
#include <iterator>
#include <mutex>
#include <utility>

namespace {
// Serializes every access to RecentBooksStore's list. Constructed at
// static-init, before setup() and therefore before any FreeRTOS task exists,
// for the same reason DownloadQueue's Worker owns its mutex at namespace
// scope rather than creating it on first use: a lazily created lock has a
// first-use race of its own, which defeats the point.
//
// Deliberately not PersistableStoreBase::storeMutex, which that header
// forbids taking on a read path -- and not recursive, so no method below may
// call another that locks. Lock order, held nowhere in reverse: this ->
// storeMutex (saveToFile) -> HalStorage::storageMutex. HalStorage never
// calls back into this store, so the SD end of that chain cannot close it.
std::mutex listMutex;
}  // namespace

void RecentBooksStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : recentBooks) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.path;
    obj["title"] = book.title;
    obj["author"] = book.author;
    obj["coverBmpPath"] = book.coverBmpPath;
    // Omitted while unknown, so an entry that has never been read stays as
    // small as it was before this field existed.
    if (book.progressPercent >= 0) {
      obj["progressPercent"] = book.progressPercent;
    }
    // Omitted for a local entry, so an existing recent.json round-trips
    // byte-for-byte and every pre-existing entry keeps reading as local.
    if (!book.remoteId.empty()) {
      obj["remoteId"] = book.remoteId;
      obj["sizeBytes"] = book.sizeBytes;
    }
  }
}

bool RecentBooksStore::fromJson(JsonVariantConst doc) {
  // Tolerate a missing/invalid 'books' key (treat as empty list); only a
  // JSON parse error is fatal. A null JsonArray iterates zero times.
  recentBooks.clear();
  JsonArrayConst arr = doc["books"].as<JsonArrayConst>();
  constexpr size_t maxEntries = static_cast<size_t>(MAX_RECENT_BOOKS) + static_cast<size_t>(MAX_REMOTE_RECENT_BOOKS);
  recentBooks.reserve(std::min(arr.size(), maxEntries));
  // The same two budgets trimLocked() applies, for the same reason: a file
  // whose front is full of remote entries must not load as a list with the
  // reader's history truncated off the end -- the next save would write that
  // truncation back over recent.json.
  recent_discovery::TrimBudget budget{static_cast<size_t>(MAX_RECENT_BOOKS),
                                      static_cast<size_t>(MAX_REMOTE_RECENT_BOOKS)};
  for (JsonObjectConst obj : arr) {
    // recentBooks.size(), not getCount(): this runs under storeMutex, and
    // getCount() takes the list lock -- the one ordering this file must not
    // invert (see listMutex above).
    if (recentBooks.size() >= maxEntries) break;
    RecentBook book;
    book.path = obj["path"] | "";
    book.title = obj["title"] | "";
    book.author = obj["author"] | "";
    book.coverBmpPath = obj["coverBmpPath"] | "";
    // Missing (pre-existing store) reads as empty, i.e. local.
    book.remoteId = obj["remoteId"] | "";
    book.sizeBytes = obj["sizeBytes"] | static_cast<uint64_t>(0);
    // A local entry with no cached percentage defaults to 0 ("not started"),
    // not -1 ("new"): -1 is what drives the NEW badge, and every entry in a
    // recent.json written before this field existed is local, so without
    // this split every book a reader has already read would render as
    // just-downloaded until they reopened it. A remote entry keeps -1 --
    // for a book this device has not downloaded, "no percentage" is the
    // truthful signal, not a migration artifact.
    const int defaultPercent = book.remoteId.empty() ? 0 : -1;
    // Out of range reads as unknown regardless of the default above.
    const int percent = obj["progressPercent"] | defaultPercent;
    book.progressPercent = (percent < 0 || percent > 100) ? -1 : percent;
    if (!budget.keep(!book.remoteId.empty())) continue;
    recentBooks.push_back(book);
  }

  LOG_DBG("RBS", "Recent books loaded from file (%d entries)", static_cast<int>(recentBooks.size()));
  return true;
}

size_t RecentBooksStore::indexOfLocked(const std::string& path) const {
  for (size_t i = 0; i < recentBooks.size(); i++) {
    if (recentBooks[i].path == path) return i;
  }
  return recentBooks.size();
}

bool RecentBooksStore::pruneMissingLocked() {
  const size_t before = recentBooks.size();
  recentBooks.erase(std::remove_if(recentBooks.begin(), recentBooks.end(), &isMissing), recentBooks.end());
  return recentBooks.size() != before;
}

void RecentBooksStore::trimLocked() {
  recent_discovery::TrimBudget budget{static_cast<size_t>(MAX_RECENT_BOOKS),
                                      static_cast<size_t>(MAX_REMOTE_RECENT_BOOKS)};
  size_t write = 0;
  for (size_t read = 0; read < recentBooks.size(); read++) {
    if (!budget.keep(!recentBooks[read].remoteId.empty())) continue;
    if (write != read) recentBooks[write] = std::move(recentBooks[read]);
    write++;
  }
  recentBooks.resize(write);
}

void RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& coverBmpPath, const int progressPercent) {
  std::lock_guard<std::mutex> lock(listMutex);

  // Drop stale entries first so a new add can't evict a valid book in their stead.
  pruneMissingLocked();

  int percent = std::clamp(progressPercent, -1, 100);

  const size_t index = indexOfLocked(path);
  if (index != recentBooks.size()) {
    // Reopening a book must not throw away the percentage its last session cached.
    if (percent < 0) {
      percent = recentBooks[index].progressPercent;
    }
    recentBooks.erase(recentBooks.begin() + static_cast<std::ptrdiff_t>(index));
  }

  // Add to front
  recentBooks.insert(recentBooks.begin(), {path, title, author, coverBmpPath, percent});
  trimLocked();

  saveToFile();
}

void RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string& author,
                                  const std::string& coverBmpPath) {
  std::lock_guard<std::mutex> lock(listMutex);
  const size_t index = indexOfLocked(path);
  if (index == recentBooks.size()) {
    return;
  }
  RecentBook& book = recentBooks[index];
  book.title = title;
  book.author = author;
  book.coverBmpPath = coverBmpPath;
  saveToFile();
}

void RecentBooksStore::addRemoteBook(const std::string& id, const std::string& path, const uint64_t sizeBytes) {
  std::lock_guard<std::mutex> lock(listMutex);
  const size_t index = indexOfLocked(path);
  if (index != recentBooks.size()) {
    recentBooks.erase(recentBooks.begin() + static_cast<std::ptrdiff_t>(index));
  }

  RecentBook book;
  book.path = path;
  book.remoteId = id;
  book.sizeBytes = sizeBytes;
  recentBooks.insert(recentBooks.begin(), book);
  trimLocked();

  saveToFile();
}

void RecentBooksStore::markDownloaded(const std::string& path) {
  std::lock_guard<std::mutex> lock(listMutex);
  const size_t index = indexOfLocked(path);
  if (index == recentBooks.size() || recentBooks[index].remoteId.empty()) {
    return;
  }
  recentBooks[index].remoteId.clear();
  recentBooks[index].sizeBytes = 0;
  saveToFile();
}

bool RecentBooksStore::removeByPath(const std::string& path) {
  std::lock_guard<std::mutex> lock(listMutex);
  const size_t index = indexOfLocked(path);
  if (index == recentBooks.size()) {
    return false;
  }
  recentBooks.erase(recentBooks.begin() + static_cast<std::ptrdiff_t>(index));
  if (!saveToFile()) {
    LOG_ERR("RBS", "Failed to persist removal of recent book: %s", path.c_str());
  }
  return true;
}

void RecentBooksStore::updateProgressPercent(const std::string& path, const int progressPercent) {
  if (progressPercent < 0) {
    return;
  }
  const int percent = std::min(progressPercent, 100);
  std::lock_guard<std::mutex> lock(listMutex);
  const size_t index = indexOfLocked(path);
  if (index == recentBooks.size() || recentBooks[index].progressPercent == percent) {
    return;
  }
  recentBooks[index].progressPercent = percent;
  if (!saveToFile()) {
    LOG_ERR("RBS", "Failed to persist progress for recent book: %s", path.c_str());
  }
}

void RecentBooksStore::updatePath(const std::string& oldPath, const std::string& newPath,
                                  const std::string& oldCachePath, const std::string& newCachePath) {
  std::lock_guard<std::mutex> lock(listMutex);
  const size_t index = indexOfLocked(oldPath);
  if (index == recentBooks.size()) {
    return;
  }
  RecentBook& book = recentBooks[index];
  book.path = newPath;
  if (!oldCachePath.empty() && !book.coverBmpPath.empty() && book.coverBmpPath.rfind(oldCachePath, 0) == 0) {
    book.coverBmpPath = newCachePath + book.coverBmpPath.substr(oldCachePath.size());
  }
  saveToFile();
}

std::vector<RecentBook> RecentBooksStore::getBooks() const {
  std::lock_guard<std::mutex> lock(listMutex);
  return recentBooks;
}

int RecentBooksStore::getCount() const {
  std::lock_guard<std::mutex> lock(listMutex);
  return static_cast<int>(recentBooks.size());
}

// Takes no lock: it reads only the caller's own RecentBook, never the list.
bool RecentBooksStore::isMissing(const RecentBook& book) {
  // A remote entry has no local file by definition -- it has not arrived yet,
  // it is not missing. Without this exemption pruneMissing(), which addBook()
  // runs before every insert, erases every new book the moment the reader
  // opens anything. Do not "simplify" this back to the bare exists() check.
  // The decision itself is host-tested in lib/RecentDiscovery, since this
  // function cannot be reached from a host build.
  return recent_discovery::shouldPrune(!book.remoteId.empty(), Storage.exists(book.path.c_str()));
}

bool RecentBooksStore::pruneMissing() {
  std::lock_guard<std::mutex> lock(listMutex);
  if (!pruneMissingLocked()) {
    return false;
  }
  if (!saveToFile()) {
    LOG_ERR("RBS", "Failed to persist pruned recent books");
  }
  return true;
}

// Takes no lock either, and must not: it touches no list state, and it opens
// and parses an EPUB, which is far too long to hold the list against.
RecentBook RecentBooksStore::getDataFromBook(std::string path) const {
  std::string lastBookFileName = "";
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash != std::string::npos) {
    lastBookFileName = path.substr(lastSlash + 1);
  }

  LOG_DBG("RBS", "Loading recent book: %s", path.c_str());

  // If epub, try to load the metadata for title/author and cover.
  // Use buildIfMissing=false to avoid heavy epub loading on boot; getTitle()/getAuthor() may be
  // blank until the book is opened, and entries with missing title are omitted from recent list.
  if (FsHelpers::hasEpubExtension(lastBookFileName)) {
    Epub epub(path, "/.crosspoint");
    epub.load(false, true);
    return RecentBook{path, epub.getTitle(), epub.getAuthor(), epub.getThumbBmpPath()};
  } else if (FsHelpers::hasXtcExtension(lastBookFileName)) {
    // Handle XTC file
    Xtc xtc(path, "/.crosspoint");
    if (xtc.load()) {
      return RecentBook{path, xtc.getTitle(), xtc.getAuthor(), xtc.getThumbBmpPath()};
    }
  } else if (FsHelpers::hasTxtExtension(lastBookFileName) || FsHelpers::hasMarkdownExtension(lastBookFileName)) {
    return RecentBook{path, lastBookFileName, "", ""};
  }
  return RecentBook{path, "", "", ""};
}
