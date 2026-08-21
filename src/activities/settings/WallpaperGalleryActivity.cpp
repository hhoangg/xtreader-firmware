// The browse/preview/attach half of wallpapers -- see WallpaperGalleryActivity.h
// for the shape of the screen and lib/WallpaperGallery for its decisions.
//
// Choices this file makes, and why:
//
//  - Two panel refreshes per page, never seven. The panel has no partial
//    refresh, so the placeholder grid goes out once, six thumbnails are fetched
//    behind it, and the finished grid goes out once. A page already fully
//    cached skips the placeholder pass entirely.
//  - Thumbnails are cached under /.crosspoint/wpthumb, NOT in /.sleep. A
//    134x223 thumbnail in /.sleep would be drawn as a lock screen by
//    SleepActivity and would be invisible to wallpaper_reconcile::plan(), which
//    only recognises its own "cpw_" names. See WallpaperGalleryPaths.h.
//  - The full-size preview streams into ONE fixed scratch file, reused by every
//    preview and removed on exit, so previewing a hundred wallpapers costs one
//    96 KB file rather than a hundred.
//  - Attaching moves that scratch file into /.sleep rather than downloading the
//    same 96 KB a second time. The move goes through the same ".part" rename
//    discipline src/sync/WallpaperSync.cpp uses, so a failure can only leave a
//    discardable ".part" behind, never a truncated .bmp where SleepActivity
//    would try to draw it.
//  - Wi-Fi comes up through WifiSelectionActivity and the exit does a
//    silentRestart(), same as FontDownloadActivity and the other network
//    screens: a wifi session fragments the heap badly enough that the cheapest
//    fix is a reboot back to Home.

#include "WallpaperGalleryActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WallpaperGalleryPaths.h>
#include <WallpaperPaths.h>
#include <WiFi.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "SyncCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "sync/WallpaperSync.h"

namespace {

constexpr char LOG_TAG[] = "WGAL";

// Longest filename SdFat will hand back for a directory entry, matching
// src/sync/WallpaperSync.cpp's own scan buffer. Heap, not stack: AGENTS.md's
// Resource Protocol caps a local at 256 bytes and this is exactly at that line.
constexpr size_t MAX_FILE_NAME_LEN = 256;

// A pathological cache directory must not turn a scan into an unbounded
// vector. Comfortably above MAX_CACHED_THUMBS; a backstop, not a policy.
constexpr size_t MAX_SCANNED_NAMES = 256;

// Corner mark on a tile that is already on the device: a filled box with a
// white tick, sized off the art so it stays proportional when the art shrinks.
constexpr int MARK_DIVISOR = 4;
constexpr int MARK_MIN = 14;
constexpr int MARK_MAX = 22;

// Border weights. The selected tile is marked by a thicker frame, never by
// inverting the art (approved mockup).
constexpr int BORDER_NORMAL = 1;
constexpr int BORDER_SELECTED = 3;

bool statusOk(const int status) { return status >= 200 && status < 300; }

}  // namespace

WallpaperGalleryActivity::WallpaperGalleryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("WallpaperGallery", renderer, mappedInput) {}

// --- Lifecycle --------------------------------------------------------------

void WallpaperGalleryActivity::onEnter() {
  Activity::onEnter();
  entries_.reserve(MAX_ENTRIES);
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void WallpaperGalleryActivity::onExit() {
  Activity::onExit();

  // The preview scratch is the only file this screen leaves behind that nothing
  // else will ever read; the thumbnail cache is deliberately kept.
  const std::string scratch =
      wallpaper_gallery_paths::joinPath(wallpaper_gallery_paths::THUMB_DIR, wallpaper_gallery_paths::PREVIEW_NAME);
  Storage.remove(scratch.c_str());
  Storage.remove(wallpaper_gallery_paths::tempNameFor(scratch).c_str());

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void WallpaperGalleryActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }
  if (!SYNC_STORE.isPaired()) {
    failWith(StrId::STR_WALLPAPER_NEEDS_PAIRING);
    return;
  }
  reloadTab();
}

// --- Tabs and data ----------------------------------------------------------

wallpaper_gallery::Sort WallpaperGalleryActivity::tabAt(const int index) {
  switch (index) {
    case 0:
      return wallpaper_gallery::Sort::Mine;
    case 2:
      return wallpaper_gallery::Sort::Recent;
    default:
      return wallpaper_gallery::Sort::Popular;
  }
}

int WallpaperGalleryActivity::tabIndex(const wallpaper_gallery::Sort sort) {
  switch (sort) {
    case wallpaper_gallery::Sort::Mine:
      return 0;
    case wallpaper_gallery::Sort::Recent:
      return 2;
    default:
      return 1;
  }
}

const char* WallpaperGalleryActivity::tabLabel(const int index) const {
  switch (index) {
    case 0:
      return tr(STR_WALLPAPERS_ON_DEVICE);
    case 2:
      return tr(STR_WALLPAPERS_NEWEST);
    default:
      return tr(STR_WALLPAPERS_POPULAR);
  }
}

int WallpaperGalleryActivity::anchorIndex() const {
  if (entries_.empty()) return 0;
  if (ring_ > RING_TABS) return ring_ - 1;
  return std::clamp(anchorTile_, 0, entryCount() - 1);
}

const wallpaper_gallery::Entry* WallpaperGalleryActivity::selectedEntry() const {
  const int index = selectedIndex();
  if (index < 0 || index >= entryCount()) return nullptr;
  return &entries_[static_cast<size_t>(index)];
}

// Collects rows off the feed stream, bounded at MAX_ENTRIES.
namespace {
struct CollectCtx {
  std::vector<wallpaper_gallery::Entry>* entries;
  size_t maxEntries;
};

bool onFeedEntry(void* ctxPtr, const wallpaper_gallery::Entry& entry) {
  auto* ctx = static_cast<CollectCtx*>(ctxPtr);
  if (entry.deleted) return true;  // a tombstone is not a listing
  if (!wallpaper_gallery_paths::isValidId(entry.id)) return true;
  if (ctx->entries->size() >= ctx->maxEntries) return true;
  ctx->entries->push_back(entry);
  return true;
}
}  // namespace

bool WallpaperGalleryActivity::fetchNextPage() {
  const std::string url = SYNC_STORE.getBaseUrl() + wallpaper_gallery::requestPath(tab_, nextCursor_, PAGE_LIMIT);
  LOG_DBG(LOG_TAG, "Fetching wallpaper page (heap: %u)", static_cast<unsigned>(ESP.getFreeHeap()));

  CollectCtx collect{&entries_, MAX_ENTRIES};
  wallpaper_gallery::StreamParser parser(&onFeedEntry, nullptr, &collect);

  int httpStatus = -1;
  const bool fetchOk = HttpDownloader::fetchUrl(
      url, [&parser](const uint8_t* data, size_t len) -> bool { return parser.feed(data, len); }, "", "", &httpStatus,
      SYNC_STORE.getAccessToken(), sync_trigger::EXPLICIT_SYNC_TIMEOUT_MS);

  if (!fetchOk || httpStatus != 200 || !parser.hasTrailer()) {
    LOG_ERR(LOG_TAG, "Gallery page fetch failed (ok=%d status=%d trailer=%d)", fetchOk, httpStatus,
            static_cast<int>(parser.hasTrailer()));
    return false;
  }

  const ManifestTrailer& trailer = parser.trailer();
  // Stop paging once the in-memory bound is reached: the rows past it would be
  // dropped by onFeedEntry anyway, so fetching them costs bytes for nothing.
  hasMore_ = trailer.hasNextCursor && !trailer.nextCursor.empty() && entries_.size() < MAX_ENTRIES;
  nextCursor_ = hasMore_ ? trailer.nextCursor : std::string();

  // The manifest tab lists what is assigned to THIS device, so every row on it
  // is attached by definition; the gallery tabs get the server's flag. Either
  // way it is ORed with what is actually in /.sleep, so a wallpaper attached a
  // moment ago shows its mark without a refetch.
  for (wallpaper_gallery::Entry& entry : entries_) {
    if (tab_ == wallpaper_gallery::Sort::Mine) entry.attached = true;
    if (!entry.attached) entry.attached = isOnDevice(entry.id);
  }
  return true;
}

void WallpaperGalleryActivity::reloadTab() {
  {
    RenderLock lock(*this);
    state_ = State::Loading;
    entries_.clear();
    nextCursor_.clear();
    hasMore_ = false;
    ring_ = 1;
    anchorTile_ = 0;
    thumbsPending_ = false;
  }
  requestUpdateAndWait();

  if (!fetchNextPage()) {
    failWith(StrId::STR_WALLPAPER_LOAD_FAILED);
    return;
  }

  {
    RenderLock lock(*this);
    state_ = State::Grid;
    // An empty tab has no tile to sit on, so the tab band keeps the focus.
    ring_ = entries_.empty() ? RING_TABS : 1;
  }
  ensurePageThumbs();
}

void WallpaperGalleryActivity::stepTab(const int direction) {
  const int next = (tabIndex(tab_) + direction + TAB_COUNT) % TAB_COUNT;
  tab_ = tabAt(next);
  reloadTab();
}

std::vector<std::string> WallpaperGalleryActivity::pageIds() const {
  std::vector<std::string> ids;
  const int start = wallpaper_grid::pageStart(anchorIndex());
  const int count = wallpaper_grid::slotsOnPage(start, entryCount());
  ids.reserve(static_cast<size_t>(std::max(count, 0)));
  for (int i = 0; i < count; i++) {
    ids.push_back(entries_[static_cast<size_t>(start + i)].id);
  }
  return ids;
}

// --- Files ------------------------------------------------------------------

bool WallpaperGalleryActivity::isOnDevice(const std::string& id) {
  const std::string name = wallpaper_paths::fileNameForId(id);
  if (name.empty()) return false;
  const std::string path = wallpaper_paths::joinPath(wallpaper_sync::SLEEP_DIR, name);
  return Storage.exists(path.c_str());
}

std::string WallpaperGalleryActivity::thumbPathFor(const std::string& id) {
  const std::string name = wallpaper_gallery_paths::thumbNameForId(id);
  if (name.empty()) return "";
  return wallpaper_gallery_paths::joinPath(wallpaper_gallery_paths::THUMB_DIR, name);
}

bool WallpaperGalleryActivity::downloadTo(const std::string& url, const char* dir, const std::string& fileName,
                                          const uint64_t expectedSize) {
  if (fileName.empty()) {
    LOG_ERR(LOG_TAG, "Refusing a filename with an unusable shape");
    return false;
  }
  if (!Storage.exists(dir) && !Storage.mkdir(dir)) {
    LOG_ERR(LOG_TAG, "Failed to create %s", dir);
    return false;
  }

  const std::string destPath = wallpaper_gallery_paths::joinPath(dir, fileName);
  const std::string tmpPath = wallpaper_gallery_paths::joinPath(dir, wallpaper_gallery_paths::tempNameFor(fileName));
  Storage.remove(tmpPath.c_str());  // discard any stale attempt

  HalFile tmpFile;
  if (!Storage.openFileForWrite(LOG_TAG, tmpPath, tmpFile)) {
    LOG_ERR(LOG_TAG, "Failed to open %s for writing", tmpPath.c_str());
    return false;
  }

  uint64_t written = 0;
  bool writeFailed = false;
  int httpStatus = -1;
  const bool fetchOk = HttpDownloader::fetchUrl(
      url,
      [this, &tmpFile, &written, &writeFailed](const uint8_t* data, size_t len) -> bool {
        if (tmpFile.write(data, len) != len) {
          writeFailed = true;
          return false;
        }
        written += len;
        // The render task is blocked behind this call, so Back has to be
        // sampled here or a stalled download could not be escaped.
        mappedInput.update();
        if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
            mappedInput.wasPressed(MappedInputManager::Button::Back)) {
          cancelRequested_ = true;
        }
        return !cancelRequested_;
      },
      "", "", &httpStatus, SYNC_STORE.getAccessToken(), sync_trigger::EXPLICIT_SYNC_TIMEOUT_MS);

  // Close before the remove()/rename() below touch the same paths (see
  // AGENTS.md's DESTRUCTOR_CLOSES_FILE "close before delete/reopen" cases).
  tmpFile.flush();
  tmpFile.close();

  const bool sizeOk = expectedSize == 0 || written == expectedSize;
  if (!fetchOk || httpStatus != 200 || writeFailed || written == 0 || !sizeOk) {
    LOG_ERR(LOG_TAG, "Download failed (ok=%d status=%d write=%d got=%llu want=%llu)", fetchOk, httpStatus,
            static_cast<int>(writeFailed), static_cast<unsigned long long>(written),
            static_cast<unsigned long long>(expectedSize));
    Storage.remove(tmpPath.c_str());
    return false;
  }

  Storage.remove(destPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), destPath.c_str())) {
    LOG_ERR(LOG_TAG, "Failed to rename %s into place", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}

// The two-pass render this whole screen is built around. Pass one puts the grid
// up with placeholder frames and a progress line; pass two replaces it with the
// finished grid. A page whose thumbnails are all cached does neither and simply
// renders once.
void WallpaperGalleryActivity::ensurePageThumbs() {
  const std::vector<std::string> ids = pageIds();

  std::vector<std::string> missing;
  missing.reserve(ids.size());
  for (const std::string& id : ids) {
    const std::string path = thumbPathFor(id);
    if (path.empty() || Storage.exists(path.c_str())) continue;
    missing.push_back(id);
  }

  if (missing.empty()) {
    {
      RenderLock lock(*this);
      thumbsPending_ = false;
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    thumbsPending_ = true;
    thumbsNeeded_ = static_cast<int>(ids.size());
    thumbsReady_ = thumbsNeeded_ - static_cast<int>(missing.size());
    cancelRequested_ = false;
  }
  requestUpdateAndWait();  // pass one: placeholders + "n of 6"

  const std::string base = SYNC_STORE.getBaseUrl();
  for (const std::string& id : missing) {
    if (cancelRequested_) break;
    const std::string url = base + "/wallpapers/" + id + "/file?variant=thumb";
    // No size check: the contract gives no per-variant byte count, and a
    // thumbnail that fails to parse simply renders as an empty frame.
    if (!downloadTo(url, wallpaper_gallery_paths::THUMB_DIR, wallpaper_gallery_paths::thumbNameForId(id), 0)) {
      LOG_ERR(LOG_TAG, "Thumbnail for %s did not arrive", id.c_str());
    }
  }

  trimThumbCache();

  {
    RenderLock lock(*this);
    thumbsPending_ = false;
  }
  requestUpdateAndWait();  // pass two: the finished grid
}

void WallpaperGalleryActivity::trimThumbCache() {
  if (!Storage.exists(wallpaper_gallery_paths::THUMB_DIR)) return;

  HalFile handle = Storage.open(wallpaper_gallery_paths::THUMB_DIR);
  if (!handle || !handle.isDirectory()) return;

  auto name = makeUniqueNoThrow<char[]>(MAX_FILE_NAME_LEN);
  if (!name) {
    LOG_ERR(LOG_TAG, "OOM: %u byte filename buffer", static_cast<unsigned>(MAX_FILE_NAME_LEN));
    return;
  }

  std::vector<std::string> localNames;
  localNames.reserve(wallpaper_gallery_paths::MAX_CACHED_THUMBS);
  for (auto entry = handle.openNextFile(); entry; entry = handle.openNextFile()) {
    if (entry.isDirectory()) continue;
    name[0] = '\0';
    entry.getName(name.get(), MAX_FILE_NAME_LEN);
    if (name[0] == '\0') continue;
    if (wallpaper_gallery_paths::classifyFileName(name.get()) == wallpaper_gallery_paths::FileKind::Unmanaged) continue;
    if (localNames.size() >= MAX_SCANNED_NAMES) break;
    localNames.emplace_back(name.get());
  }
  handle.close();

  for (const std::string& doomed : wallpaper_gallery_paths::planCacheEviction(localNames, pageIds())) {
    const std::string path = wallpaper_gallery_paths::joinPath(wallpaper_gallery_paths::THUMB_DIR, doomed);
    if (!Storage.remove(path.c_str())) LOG_ERR(LOG_TAG, "Failed to remove %s", path.c_str());
  }
}

// --- Preview and attachment -------------------------------------------------

void WallpaperGalleryActivity::openPreview() {
  const wallpaper_gallery::Entry* entry = selectedEntry();
  if (!entry) return;

  const std::string id = entry->id;
  const uint64_t sizeBytes = entry->sizeBytes;
  // The server's `attached` is not enough: it can be true for a wallpaper whose
  // file has not been synced down yet. Only an actual file on the card spares
  // the download.
  if (isOnDevice(id)) {
    // Already on the card at full size -- draw that copy rather than spending
    // 96 KB and a few seconds fetching a byte-identical one.
    const std::string path = wallpaper_paths::joinPath(wallpaper_sync::SLEEP_DIR, wallpaper_paths::fileNameForId(id));
    RenderLock lock(*this);
    previewPath_ = path;
    state_ = State::Preview;
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state_ = State::Loading;
    cancelRequested_ = false;
  }
  requestUpdateAndWait();

  const std::string url = SYNC_STORE.getBaseUrl() + "/wallpapers/" + id + "/file";
  if (!downloadTo(url, wallpaper_gallery_paths::THUMB_DIR, wallpaper_gallery_paths::PREVIEW_NAME, sizeBytes)) {
    if (cancelRequested_) {
      RenderLock lock(*this);
      state_ = State::Grid;
      requestUpdate();
      return;
    }
    failWith(StrId::STR_WALLPAPER_PREVIEW_FAILED);
    return;
  }

  RenderLock lock(*this);
  previewPath_ =
      wallpaper_gallery_paths::joinPath(wallpaper_gallery_paths::THUMB_DIR, wallpaper_gallery_paths::PREVIEW_NAME);
  state_ = State::Preview;
  requestUpdate();
}

bool WallpaperGalleryActivity::attachSelected(const wallpaper_gallery::Entry& entry) {
  std::string response;
  int status = -1;
  const std::string url = SYNC_STORE.getBaseUrl() + "/wallpapers/" + entry.id + "/attach";
  if (!HttpDownloader::postJson(url, "{}", response, &status, SYNC_STORE.getAccessToken(),
                                sync_trigger::EXPLICIT_SYNC_TIMEOUT_MS) ||
      !statusOk(status)) {
    LOG_ERR(LOG_TAG, "Attach of %s failed (status=%d)", entry.id.c_str(), status);
    return false;
  }

  const std::string fileName = wallpaper_paths::fileNameForId(entry.id);
  if (fileName.empty()) return false;
  if (!Storage.exists(wallpaper_sync::SLEEP_DIR) && !Storage.mkdir(wallpaper_sync::SLEEP_DIR)) {
    LOG_ERR(LOG_TAG, "Failed to create %s", wallpaper_sync::SLEEP_DIR);
    return false;
  }

  const std::string destPath = wallpaper_paths::joinPath(wallpaper_sync::SLEEP_DIR, fileName);
  const std::string tmpPath =
      wallpaper_paths::joinPath(wallpaper_sync::SLEEP_DIR, wallpaper_paths::tempNameFor(fileName));
  const std::string scratch =
      wallpaper_gallery_paths::joinPath(wallpaper_gallery_paths::THUMB_DIR, wallpaper_gallery_paths::PREVIEW_NAME);

  // The preview already pulled the full image; move it rather than fetching the
  // identical 96 KB again. Via ".part" so a failed second rename cannot leave a
  // half-named file where SleepActivity would draw it.
  Storage.remove(tmpPath.c_str());
  bool staged = Storage.exists(scratch.c_str()) && Storage.rename(scratch.c_str(), tmpPath.c_str());
  if (staged) {
    Storage.remove(destPath.c_str());
    if (!Storage.rename(tmpPath.c_str(), destPath.c_str())) {
      Storage.remove(tmpPath.c_str());
      staged = false;
    }
  }
  if (!staged) {
    const std::string url2 = SYNC_STORE.getBaseUrl() + "/wallpapers/" + entry.id + "/file";
    if (!downloadTo(url2, wallpaper_sync::SLEEP_DIR, fileName, entry.sizeBytes)) return false;
  }
  return true;
}

bool WallpaperGalleryActivity::detachSelected(const wallpaper_gallery::Entry& entry) {
  std::string response;
  int status = -1;
  const std::string url = SYNC_STORE.getBaseUrl() + "/wallpapers/" + entry.id + "/attach";
  if (!HttpDownloader::deleteResource(url, response, &status, SYNC_STORE.getAccessToken()) || !statusOk(status)) {
    LOG_ERR(LOG_TAG, "Detach of %s failed (status=%d)", entry.id.c_str(), status);
    return false;
  }

  const std::string fileName = wallpaper_paths::fileNameForId(entry.id);
  if (!fileName.empty()) {
    const std::string path = wallpaper_paths::joinPath(wallpaper_sync::SLEEP_DIR, fileName);
    if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) {
      LOG_ERR(LOG_TAG, "Removed %s server-side but not from the card", path.c_str());
    }
  }
  return true;
}

void WallpaperGalleryActivity::toggleAttachment() {
  const int index = selectedIndex();
  if (index < 0 || index >= entryCount()) return;
  const bool wasAttached = entries_[static_cast<size_t>(index)].attached;

  {
    RenderLock lock(*this);
    state_ = State::Loading;
  }
  requestUpdateAndWait();

  const wallpaper_gallery::Entry snapshot = entries_[static_cast<size_t>(index)];
  const bool ok = wasAttached ? detachSelected(snapshot) : attachSelected(snapshot);
  if (!ok) {
    failWith(wasAttached ? StrId::STR_WALLPAPER_REMOVE_FAILED : StrId::STR_WALLPAPER_ADD_FAILED);
    return;
  }

  RenderLock lock(*this);
  if (index < entryCount()) entries_[static_cast<size_t>(index)].attached = !wasAttached;
  // Back to the grid: the tile's corner mark is the whole point of the change,
  // and the preview would otherwise sit there looking unchanged.
  previewPath_.clear();
  state_ = State::Grid;
  requestUpdate();
}

void WallpaperGalleryActivity::failWith(const StrId messageId) {
  RenderLock lock(*this);
  errorMessage_ = I18N.get(messageId);
  state_ = State::Error;
  requestUpdate();
}

// --- Input ------------------------------------------------------------------

void WallpaperGalleryActivity::moveRingTo(const int ring) {
  const int previousPage = wallpaper_grid::pageIndex(anchorIndex());
  ring_ = ring;
  if (ring_ > RING_TABS) anchorTile_ = ring_ - 1;
  const int page = wallpaper_grid::pageIndex(anchorIndex());

  if (wallpaper_grid::needsNextPage(anchorIndex(), entryCount(), hasMore_)) {
    {
      RenderLock lock(*this);
      state_ = State::Loading;
    }
    requestUpdateAndWait();
    if (!fetchNextPage()) {
      failWith(StrId::STR_WALLPAPER_LOAD_FAILED);
      return;
    }
    RenderLock lock(*this);
    state_ = State::Grid;
  }

  if (page != previousPage) {
    ensurePageThumbs();
    return;
  }
  requestUpdate();
}

void WallpaperGalleryActivity::navigateButtons() {
  const int ringCount = entryCount() + 1;  // tab band + one position per tile

  buttonNavigator.onNextRelease([this, ringCount] { moveRingTo(ButtonNavigator::nextIndex(ring_, ringCount)); });
  buttonNavigator.onPreviousRelease(
      [this, ringCount] { moveRingTo(ButtonNavigator::previousIndex(ring_, ringCount)); });

  // Hold jumps a page, exactly as UiListActivity::navigateButtons() does; on
  // the tab band there is no page to jump, so it steps the tab instead (the
  // same hand-off UiTabListActivity makes).
  buttonNavigator.onNextContinuous([this] {
    if (ring_ == RING_TABS) {
      stepTab(1);
      return;
    }
    // Already page-aligned when there is more than one page; when there is
    // not, nextPageIndex falls back to a single step, which is what
    // UiListActivity does too.
    moveRingTo(ButtonNavigator::nextPageIndex(selectedIndex(), entryCount(), wallpaper_grid::PAGE_SIZE) + 1);
  });
  buttonNavigator.onPreviousContinuous([this] {
    if (ring_ == RING_TABS) {
      stepTab(-1);
      return;
    }
    moveRingTo(ButtonNavigator::previousPageIndex(selectedIndex(), entryCount(), wallpaper_grid::PAGE_SIZE) + 1);
  });
}

bool WallpaperGalleryActivity::handleGridButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ring_ == RING_TABS) {
      stepTab(1);
    } else {
      openPreview();
    }
    return true;
  }
  return false;
}

bool WallpaperGalleryActivity::handlePreviewButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    RenderLock lock(*this);
    previewPath_.clear();
    state_ = State::Grid;
    requestUpdate();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleAttachment();
    return true;
  }
  return false;
}

bool WallpaperGalleryActivity::handleErrorButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (entries_.empty()) {
      finish();
    } else {
      RenderLock lock(*this);
      state_ = State::Grid;
      requestUpdate();
    }
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    reloadTab();
    return true;
  }
  return false;
}

void WallpaperGalleryActivity::loop() {
  switch (state_) {
    case State::Grid:
      if (handleGridButtons()) return;
      navigateButtons();
      return;
    case State::Preview:
      handlePreviewButtons();
      return;
    case State::Error:
      handleErrorButtons();
      return;
    case State::WifiSelection:
    case State::Loading:
      return;
  }
}

// --- Rendering --------------------------------------------------------------

std::string WallpaperGalleryActivity::formatSize(const uint64_t bytes) {
  char buf[24];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  }
  return buf;
}

wallpaper_grid::Bounds WallpaperGalleryActivity::contentBounds() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight;
  // One line under the grid carries the page counter (and, during the
  // placeholder pass, the thumbnail progress).
  const int statusHeight = renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - statusHeight;
  return wallpaper_grid::Bounds{metrics.contentSidePadding, top,
                                renderer.getScreenWidth() - metrics.contentSidePadding * 2, bottom - top};
}

void WallpaperGalleryActivity::drawChrome() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_WALLPAPER_GALLERY));

  std::vector<TabInfo> tabs;
  tabs.reserve(TAB_COUNT);
  for (int i = 0; i < TAB_COUNT; i++) {
    tabs.push_back(TabInfo{tabLabel(i), i == tabIndex(tab_)});
  }
  GUI.drawTabBar(renderer,
                 Rect{0, metrics.topPadding + metrics.headerHeight, renderer.getScreenWidth(), metrics.tabBarHeight},
                 tabs, ring_ == RING_TABS);
}

void WallpaperGalleryActivity::drawTile(const wallpaper_grid::Layout& layout, const int slot,
                                        const int entryIndex) const {
  const wallpaper_grid::Bounds art = wallpaper_grid::artBounds(layout, slot);
  const wallpaper_gallery::Entry& entry = entries_[static_cast<size_t>(entryIndex)];
  const bool selected = entryIndex == selectedIndex();

  // The frame. A thicker one is what marks the selection -- never inverting the
  // art, which on e-ink would render the wallpaper unrecognisable.
  const int border = selected ? BORDER_SELECTED : BORDER_NORMAL;
  renderer.drawRect(art.x - border, art.y - border, art.width + border * 2, art.height + border * 2, border, true);

  if (!thumbsPending_) {
    const std::string path = thumbPathFor(entry.id);
    HalFile file;
    if (!path.empty() && Storage.openFileForRead(LOG_TAG, path, file)) {
      Bitmap bitmap(file, true);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        // Streams row by row and allocates only two row-sized buffers, so
        // nothing here holds a decoded image.
        renderer.drawBitmap(bitmap, art.x, art.y, art.width, art.height, 0, 0);
      }
      file.close();
    }
  }

  // Corner mark for a wallpaper already on this device.
  if (entry.attached) {
    const int size = std::clamp(art.width / MARK_DIVISOR, MARK_MIN, MARK_MAX);
    const int markX = art.x + art.width - size;
    renderer.fillRect(markX, art.y, size, size, true);
    const int inset = std::max(size / 5, 2);
    renderer.drawLine(markX + inset, art.y + size / 2, markX + size / 2 - 1, art.y + size - inset, 2, false);
    renderer.drawLine(markX + size / 2 - 1, art.y + size - inset, markX + size - inset, art.y + inset, 2, false);
  }

  // Caption: the name over at most two lines, then uploader and attach count.
  const int textLeft = art.x;
  const int textWidth = art.width;
  int y = art.y + layout.tileHeight - layout.nameLineHeight * wallpaper_grid::NAME_LINES - layout.metaLineHeight;
  const char* name = entry.name.empty() ? entry.id.c_str() : entry.name.c_str();
  for (const std::string& line : renderer.wrappedText(SMALL_FONT_ID, name, textWidth, wallpaper_grid::NAME_LINES)) {
    renderer.drawText(SMALL_FONT_ID, textLeft, y, line.c_str());
    y += layout.nameLineHeight;
  }

  // On the "On Device" tab there is no uploader to name, so the file size takes
  // that line instead of leaving it blank.
  std::string meta;
  if (!entry.ownerName.empty()) {
    meta = entry.ownerName;
    // ASCII separator on purpose: the built-in UI font's coverage is fixed, so
    // a typographic middle dot would risk a missing-glyph box on some builds.
    if (entry.attachCount > 0) meta += " - " + std::to_string(entry.attachCount);
  } else if (entry.sizeBytes > 0) {
    meta = formatSize(entry.sizeBytes);
  }
  if (!meta.empty()) {
    const int metaY = art.y + layout.tileHeight - layout.metaLineHeight;
    renderer.drawText(SMALL_FONT_ID, textLeft, metaY,
                      renderer.truncatedText(SMALL_FONT_ID, meta.c_str(), textWidth).c_str());
  }
}

void WallpaperGalleryActivity::drawGrid() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const wallpaper_grid::Bounds content = contentBounds();
  const int nameLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const wallpaper_grid::Layout layout = wallpaper_grid::layout(content, nameLineHeight, nameLineHeight);

  if (entries_.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, content.y + content.height / 2, tr(STR_WALLPAPER_NONE));
    return;
  }
  if (!layout.valid) {
    renderer.drawCenteredText(UI_10_FONT_ID, content.y + content.height / 2, tr(STR_OUT_OF_BOUNDS));
    return;
  }

  const int start = wallpaper_grid::pageStart(anchorIndex());
  const int slots = wallpaper_grid::slotsOnPage(start, entryCount());
  for (int slot = 0; slot < slots; slot++) {
    drawTile(layout, slot, start + slot);
  }

  // Status line: the thumbnail progress while the placeholder pass is up,
  // otherwise which page of how many this is.
  char status[48];
  if (thumbsPending_) {
    snprintf(status, sizeof(status), tr(STR_WALLPAPER_THUMBS_PROGRESS), thumbsReady_, thumbsNeeded_);
  } else {
    const int pages = wallpaper_grid::pageCount(entryCount());
    snprintf(status, sizeof(status), "%d / %d%s", wallpaper_grid::pageIndex(start) + 1, pages, hasMore_ ? "+" : "");
  }
  const int statusY = renderer.getScreenHeight() - metrics.buttonHintsHeight - renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawCenteredText(SMALL_FONT_ID, statusY, status);
}

void WallpaperGalleryActivity::drawPreview() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int barHeight =
      renderer.getLineHeight(UI_10_FONT_ID) + renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing * 2;
  const int imageHeight = pageHeight - metrics.buttonHintsHeight - barHeight;

  HalFile file;
  bool drawn = false;
  if (!previewPath_.empty() && Storage.openFileForRead(LOG_TAG, previewPath_, file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
      // Mirror the scale drawBitmap will apply, so the horizontal centring is
      // computed against the size that actually lands on the panel.
      const float scale = std::min({static_cast<float>(pageWidth) / static_cast<float>(bitmap.getWidth()),
                                    static_cast<float>(imageHeight) / static_cast<float>(bitmap.getHeight()), 1.0f});
      const int width = static_cast<int>(static_cast<float>(bitmap.getWidth()) * scale);
      renderer.drawBitmap(bitmap, (pageWidth - width) / 2, 0, pageWidth, imageHeight, 0, 0);
      drawn = true;
    }
    file.close();
  }
  if (!drawn) {
    renderer.drawCenteredText(UI_10_FONT_ID, imageHeight / 2, tr(STR_WALLPAPER_PREVIEW_FAILED));
  }

  const wallpaper_gallery::Entry* entry = selectedEntry();
  if (!entry) return;

  // Bottom bar: name, then uploader and size.
  const int barTop = imageHeight;
  renderer.drawLine(0, barTop, pageWidth, barTop, true);
  const char* name = entry->name.empty() ? entry->id.c_str() : entry->name.c_str();
  renderer.drawCenteredText(
      UI_10_FONT_ID, barTop + metrics.verticalSpacing / 2,
      renderer.truncatedText(UI_10_FONT_ID, name, pageWidth - metrics.contentSidePadding * 2, EpdFontFamily::BOLD)
          .c_str(),
      true, EpdFontFamily::BOLD);

  std::string meta = entry->ownerName;
  if (entry->sizeBytes > 0) {
    if (!meta.empty()) meta += " - ";
    meta += formatSize(entry->sizeBytes);
  }
  if (!meta.empty()) {
    renderer.drawCenteredText(
        SMALL_FONT_ID, barTop + metrics.verticalSpacing / 2 + renderer.getLineHeight(UI_10_FONT_ID),
        renderer.truncatedText(SMALL_FONT_ID, meta.c_str(), pageWidth - metrics.contentSidePadding * 2).c_str());
  }
}

void WallpaperGalleryActivity::drawFooter() const {
  // Back / Confirm / Left / Right, in that order. The order is fixed by
  // hardware; a screen never reorders it.
  const char* confirm = "";
  const char* previous = "";
  const char* next = "";
  switch (state_) {
    case State::Grid:
      confirm = entries_.empty() ? "" : (ring_ == RING_TABS ? tr(STR_WALLPAPERS_NEXT_TAB) : tr(STR_SELECT));
      previous = tr(STR_DIR_LEFT);
      next = tr(STR_DIR_RIGHT);
      break;
    case State::Preview: {
      const wallpaper_gallery::Entry* entry = selectedEntry();
      confirm = (entry && entry->attached) ? tr(STR_WALLPAPER_REMOVE) : tr(STR_WALLPAPER_ADD);
      break;
    }
    case State::Error:
      confirm = tr(STR_RETRY);
      break;
    case State::Loading:
    case State::WifiSelection:
      break;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, previous, next);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WallpaperGalleryActivity::render(RenderLock&&) {
  renderer.clearScreen();

  if (state_ == State::Preview) {
    // Full-screen: no header, no tab band -- the wallpaper is the content.
    drawPreview();
    drawFooter();
    renderer.displayBuffer();
    return;
  }

  drawChrome();

  switch (state_) {
    case State::Loading:
      renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_WALLPAPER_LOADING));
      break;
    case State::Grid:
      drawGrid();
      break;
    case State::Error:
      renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, errorMessage_.c_str());
      break;
    case State::Preview:
    case State::WifiSelection:
      break;
  }

  drawFooter();
  renderer.displayBuffer();
}
