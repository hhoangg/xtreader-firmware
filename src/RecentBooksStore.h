#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

struct RecentBook {
  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;
  // Whole-book reading progress, 0..100, or -1 when unknown. Cached by the
  // reader on exit so the home screen can show it without loading the spine
  // table off the SD card. Absent from an older recent.json, which reads as -1.
  int progressPercent = -1;
  // Manifest id of a book the server has that this device has not downloaded
  // yet. Empty for a book that lives on the SD card. Absent from an older
  // recent.json, which reads as empty (i.e. local).
  std::string remoteId;
  // Server-reported size in bytes for a remote entry; 0 when unknown or local.
  uint64_t sizeBytes = 0;

  bool operator==(const RecentBook& other) const { return path == other.path; }
};

/**
 * The one list of books that matter, most recent first: books the reader has
 * opened, and books the server has that this device has not downloaded yet.
 *
 * Thread safety: every public method that touches the list takes one
 * non-recursive mutex (see RecentBooksStore.cpp) and releases it before
 * returning, because the list is mutated from more than one task -- the UI
 * task opens a book, the render task merges a sync's discoveries, and the
 * download worker marks a finished transfer local. A `std::vector` of
 * `std::string`s reallocating under a concurrent reader is undefined
 * behaviour, and on this board a bad free is a reset, not an exception.
 *
 * Two consequences worth knowing before adding a method:
 *  - Public methods do not call each other. The lock is not recursive, so a
 *    public entry point takes it once and the private *Locked() helpers do
 *    the work assuming it is held.
 *  - toJson()/fromJson() must NOT take it. They run under
 *    PersistableStoreBase::storeMutex, and the lock order everywhere else is
 *    list -> store -> HalStorage; taking the list lock from inside a save
 *    would invert it. fromJson() writes the list with only storeMutex held,
 *    which is safe because loadFromFile() is called once from setup(), before
 *    any other task exists. Keep it that way.
 */
class RecentBooksStore : public PersistableStore<RecentBooksStore> {
 public:
  // How many *local* entries the list holds -- books on the SD card, each
  // carrying reading history that nothing can bring back once it is written
  // over. A remote entry never counts against this budget and never evicts
  // one of these.
  static constexpr int MAX_RECENT_BOOKS = 10;

  // How many *remote* entries (books the server has that this device has
  // not downloaded) the list holds, on a budget of their own. Small on
  // purpose: Home shows three rows, and anything trimmed here is one
  // manifest sync away from coming back. Public because discovery bounds
  // both its candidate scan and its per-sync insert count by it --
  // collecting more new books than the list could ever keep would cost SD
  // stats, and an SD write each, for rows that get trimmed on insert anyway.
  static constexpr int MAX_REMOTE_RECENT_BOOKS = 5;

 private:
  std::vector<RecentBook> recentBooks;

  RecentBooksStore() = default;
  ~RecentBooksStore() = default;

  friend class PersistableStore<RecentBooksStore>;

  // --- Unlocked internals: every one of these assumes the list lock is
  // already held by the public method that called it. None of them may be
  // made public without wrapping it, and none may call a public method.

  // Index of the entry whose path matches, or recentBooks.size() for none.
  size_t indexOfLocked(const std::string& path) const;
  // pruneMissing()'s body, without the lock or the persist. addBook() runs
  // it before its own insert so a stale entry cannot evict a valid book.
  bool pruneMissingLocked();
  // Drops whatever an insert has pushed past the caps: local and remote
  // entries are counted and trimmed independently, oldest-first within each
  // kind (see recent_discovery::TrimBudget for why they are not one cap).
  void trimLocked();

 public:
  static const char* getFilePath() { return "/.crosspoint/recent.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Add a book to the recent list (moves to front if already exists).
  // progressPercent is 0..100, or -1 to keep whatever an existing entry
  // already cached (a book being opened has no fresh percentage yet).
  void addBook(const std::string& path, const std::string& title, const std::string& author,
               const std::string& coverBmpPath, int progressPercent = -1);

  void updateBook(const std::string& path, const std::string& title, const std::string& author,
                  const std::string& coverBmpPath);

  // Add a book the server has that this device has not downloaded yet, at the
  // front of the list. If path is already present (local or remote), it is
  // replaced in place at the front rather than duplicated.
  void addRemoteBook(const std::string& id, const std::string& path, uint64_t sizeBytes);

  // Mark a remote entry as downloaded: clears remoteId (and sizeBytes) so it
  // reads as local, without moving it — a completed download is not an event
  // the reader caused, so it must not jump to the front. No-op if path is not
  // found or is already local.
  void markDownloaded(const std::string& path);

  // Remove the entry whose path matches (used when a book is removed from recents or finished/read).
  // Returns true if an entry was found and removed (no-op + false otherwise).
  // Persistence is best-effort: a failed save is logged, not reflected in the return.
  bool removeByPath(const std::string& path);

  // Cache the whole-book reading percentage (0..100) for an entry. A negative
  // percent means "not known", and leaves any cached value alone. No-op, and
  // no SD write, when nothing matches path or the value is unchanged.
  void updateProgressPercent(const std::string& path, int progressPercent);

  // Repoint an entry's path (and coverBmpPath, if it lived under the old cache dir) after the
  // backing file and cache dir were moved on disk. No-op if no entry matches oldPath.
  // Persists on success. Keeps the entry's list position (does not reorder).
  void updatePath(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                  const std::string& newCachePath);

  // True if the book's backing file is no longer present on the SD card.
  // Always false for a remote entry (non-empty remoteId): it has no local
  // file by definition, so it is never "missing".
  static bool isMissing(const RecentBook& book);

  // Remove entries whose backing file is no longer on the SD card, and
  // persist if that removed anything. Returns true if any entry was removed.
  // It persists itself deliberately: the list lock is released the moment
  // this returns, so a caller that followed it with its own saveToFile()
  // would serialize the list unlocked, racing whatever mutated it in between.
  bool pruneMissing();

  // A copy of the list, most recent first, taken under the lock.
  //
  // By value, not by reference, and that is the whole point: a reference
  // outlives the lock, and callers walk this list while doing things that
  // yield (Storage.exists() per entry, thumbnail generation), during which
  // another task can insert and reallocate the vector out from under them.
  // At MAX_RECENT_BOOKS entries the copy is on the order of a kilobyte, paid
  // on screen entry, never during a transfer.
  std::vector<RecentBook> getBooks() const;

  // Get the count of recent books
  int getCount() const;

  RecentBook getDataFromBook(std::string path) const;
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()
