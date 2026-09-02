#include "RecentBooksActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <HomeBookSlots.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "sync/DownloadQueue.h"

namespace fui = freeink::ui;

namespace {
// Hold threshold for the long-press "remove from list" action (firmware convention).
constexpr unsigned long LONG_PRESS_MS = 1000;

// Same formatting HomeActivity.cpp's formatByteSize() uses for a remote
// row's size suffix; not shared because that one is file-local to
// HomeActivity.cpp.
void formatByteSize(const uint64_t bytes, char* out, const size_t outSize) {
  if (bytes >= 1024 * 1024) {
    snprintf(out, outSize, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else {
    snprintf(out, outSize, "%.0f KB", static_cast<double>(bytes) / 1024.0);
  }
}

// A remote row's status text, in the same Home-screen wording Home's own
// rows use (see HomeActivity.cpp's formatSlotStatus, and the STR_HOME_ON_SERVER
// family of keys it draws on) -- this screen shows the same recency list, so
// it must not invent a second vocabulary for the same states. OnServer,
// JustDownloaded and Read all fall to the same default: remoteState() (called
// below) only ever returns the first of the three for a remote entry, but the
// switch stays exhaustive rather than assuming that.
void formatRemoteStatus(const RecentBook& book, const home_book_slots::QueueView& queue, char* out,
                        const size_t outSize) {
  int queuePosition = 0;
  const home_book_slots::State state = home_book_slots::remoteState(book.remoteId, queue, queuePosition);
  switch (state) {
    case home_book_slots::State::Queued:
      if (queuePosition > 0) {
        snprintf(out, outSize, tr(STR_HOME_QUEUE_POSITION), queuePosition);
      } else {
        snprintf(out, outSize, "%s", tr(STR_HOME_QUEUED));
      }
      break;
    case home_book_slots::State::Downloading:
      snprintf(out, outSize, "%s", tr(STR_HOME_DOWNLOADING_SHORT));
      break;
    case home_book_slots::State::Failed:
      snprintf(out, outSize, "%s", tr(STR_HOME_DOWNLOAD_FAILED));
      break;
    case home_book_slots::State::OnServer:
    case home_book_slots::State::JustDownloaded:
    case home_book_slots::State::Read:
    default: {
      char size[16];
      formatByteSize(book.sizeBytes, size, sizeof(size));
      snprintf(out, outSize, "%s - %s", tr(STR_HOME_ON_SERVER), size);
      break;
    }
  }
}
}  // namespace

RecentBooksActivity::RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("RecentBooks", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

home_book_slots::QueueView RecentBooksActivity::buildQueueView() const {
  // Same download_queue::Snapshot -> home_book_slots::QueueView reshape
  // HomeActivity::rebuildSlots() does: one snapshot, not one call per row.
  const download_queue::Snapshot queueSnap = download_queue::snapshot();
  home_book_slots::QueueView queue;
  queue.entries.reserve(queueSnap.count);
  for (size_t i = 0; i < queueSnap.count; i++) {
    const auto state = queueSnap.items[i].status == download_queue::ItemStatus::Downloading
                           ? home_book_slots::State::Downloading
                           : home_book_slots::State::Queued;
    queue.entries.push_back({queueSnap.items[i].id, state, static_cast<int>(i) + 1});
  }
  if (queueSnap.lastResult.hasResult && !queueSnap.lastResult.ok) {
    queue.lastFailedId = queueSnap.lastResult.id;
  }
  return queue;
}

void RecentBooksActivity::loadRecentBooks() {
  recentBooks = RECENT_BOOKS.getBooks();
  // A remote entry carries no title/author of its own -- see
  // RecentBooksStore::addRemoteBook() -- so it is derived from its path the
  // same way Home's rows derive it (HomeBookSlots.h rule 4).
  for (RecentBook& book : recentBooks) {
    if (book.remoteId.empty()) continue;
    book.title = home_book_slots::titleFromPath(book.path);
    book.author = home_book_slots::authorFromPath(book.path);
  }
  rebuildRowItems();
}

// Derives rowItems from recentBooks. Called whenever recentBooks changes
// (loadRecentBooks(), i.e. load/removal) so buildScreen() reuses the cached
// rows on every repaint instead of rebuilding them per render.
void RecentBooksActivity::rebuildRowItems() {
  rowItems.clear();
  rowItems.reserve(recentBooks.size());
  rowValues.assign(recentBooks.size(), std::string());
  // One snapshot for the whole rebuild, not one per remote row (see
  // buildQueueView()'s comment).
  const home_book_slots::QueueView queue = buildQueueView();
  for (size_t i = 0; i < recentBooks.size(); i++) {
    const RecentBook& book = recentBooks[i];
    fui::ListItem item;
    item.label = book.title.c_str();
    if (!book.author.empty()) item.subtitle = book.author.c_str();
    if (!book.remoteId.empty()) {
      char status[96];
      formatRemoteStatus(book, queue, status, sizeof(status));
      rowValues[i] = status;
      item.value = rowValues[i].c_str();
    }
    item.icon = listIconFor(UITheme::getFileIcon(book.path), 32);  // subtitle rows carry the larger icon
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }

  // One SD pass for every CJK title/author on the screen; repaints then hit
  // the resident tables instead of re-reading per-string. Titles draw bold
  // (see buildScreen), authors regular — separate per-style prewarms. Getter
  // form: no concatenated copy (a bare-new string append aborts under heap
  // pressure). See GfxRenderer::prewarmFallbackText().
  const auto count = static_cast<uint32_t>(recentBooks.size());
  renderer.prewarmFallbackText(
      uiScaleSpec().smallFontId,
      [](const void* ctx, uint32_t i) -> const char* {
        return (*static_cast<const std::vector<RecentBook>*>(ctx))[i].title.c_str();
      },
      &recentBooks, count, EpdFontFamily::BOLD);
  renderer.prewarmFallbackText(
      uiScaleSpec().smallFontId,
      [](const void* ctx, uint32_t i) -> const char* {
        return (*static_cast<const std::vector<RecentBook>*>(ctx))[i].author.c_str();
      },
      &recentBooks, count);
}

void RecentBooksActivity::onEnter() {
  UiListActivity::onEnter();

  // Prune entries whose backing files are gone; this is one of two interaction
  // points where the persistent store gets cleaned (the other is addBook).
  // It persists itself -- saving from here would serialize the list after the
  // store's lock had been released.
  RECENT_BOOKS.pruneMissing();

  loadRecentBooks();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  // rowItems' label/subtitle/value pointers alias recentBooks'/rowValues'
  // strings; drop all three.
  rowItems.clear();
  rowValues.clear();
  recentBooks.clear();
}

void RecentBooksActivity::activateIndex(const int index) {
  // The interaction table can deliver a row index captured before a removal
  // shrank the list; the next render re-registers the rows.
  if (index < 0 || index >= listCount()) return;
  const RecentBook& book = recentBooks[index];
  if (!book.remoteId.empty()) {
    activateRemote(book);
    return;
  }
  // Opening the book leaves this screen; a lingering flash would gray an
  // unrelated row when the list next appears.
  app.clearTapFlash();
  LOG_DBG("RBA", "Selected recent book: %s", book.path.c_str());
  onSelectBook(book.path);
}

void RecentBooksActivity::activateRemote(const RecentBook& book) {
  int queuePosition = 0;
  const home_book_slots::State state = home_book_slots::remoteState(book.remoteId, buildQueueView(), queuePosition);
  if (state == home_book_slots::State::Queued || state == home_book_slots::State::Downloading) {
    // Nothing to do, same as Home's row: only download_queue::cancelAll()
    // exists, there is no per-row cancel.
    return;
  }

  app.clearTapFlash();
  // No repaint here on success: it happens below, once, after the enqueue
  // outcome is known -- this screen has no background queue poll to pick it
  // up later the way Home's pollDownloadQueue() does.
  const download_queue::EnqueueOutcome outcome = download_queue::enqueue(book.remoteId);
  if (outcome != download_queue::EnqueueOutcome::Ok) {
    LOG_DBG("RBA", "Enqueue of %s refused (outcome=%d)", book.remoteId.c_str(), static_cast<int>(outcome));
    showEnqueueRefused(outcome);
    return;
  }
  loadRecentBooks();
  requestUpdate(true);
}

void RecentBooksActivity::showEnqueueRefused(const download_queue::EnqueueOutcome outcome) {
  if (outcome == download_queue::EnqueueOutcome::AlreadyQueued) return;

  const char* message = nullptr;
  switch (outcome) {
    case download_queue::EnqueueOutcome::NotPaired:
      message = tr(STR_HOME_ENQUEUE_NOT_PAIRED);
      break;
    case download_queue::EnqueueOutcome::Full:
      message = tr(STR_HOME_ENQUEUE_QUEUE_FULL);
      break;
    default:
      message = tr(STR_HOME_ENQUEUE_UNAVAILABLE);
      break;
  }

  RenderLock lock(*this);
  GUI.drawPopup(renderer, message);
  // Deliberately no requestUpdate(): the next input-driven redraw clears it,
  // same reasoning as HomeActivity::showEnqueueRefused().
}

void RecentBooksActivity::onRowLongPress(const int index) {
  if (index < 0 || index >= listCount()) return;
  // Long-press prompts removal from the list (mirrors the Confirm-button hold).
  app.clearTapFlash();
  promptRemoveBook(recentBooks[index].path, recentBooks[index].title);
}

bool RecentBooksActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() && nav.selected < listCount()) {
      if (mappedInput.getHeldTime() >= LONG_PRESS_MS) {
        promptRemoveBook(recentBooks[nav.selected].path, recentBooks[nav.selected].title);
      } else {
        activateIndex(nav.selected);
      }
      return true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return true;
  }

  return false;
}

// For a remote entry this only dismisses it from the recency list -- there is
// nothing else to remove: it is not on this device, RECENT_BOOKS.removeByPath()
// never touches the server or the download queue, and a download already in
// flight keeps running (its eventual markDownloaded() becomes a harmless
// no-op against a path no longer in the list -- RecentBooksStore.h). If the
// book is still on the server next sync, discovery treats it as new again and
// re-inserts it, exactly like a local book that is removed here and later
// reopened.
void RecentBooksActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("RBA", "Remove from recents cancelled");
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      LOG_DBG("RBA", "Removed from recents: %s", path.c_str());
      // The interaction table still indexes the pre-removal rows; stop routing
      // touches against it until the next render republishes.
      closeRouting();
      loadRecentBooks();
      if (recentBooks.empty()) {
        nav.selected = 0;
      } else if (nav.selected >= listCount()) {
        nav.selected = listCount() - 1;
      }
      nav.follow(listCount());
      requestUpdate(true);
    }
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title),
      std::move(handler));
}

void RecentBooksActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (recentBooks.empty()) {
    screen.centeredText(tr(STR_NO_RECENT_BOOKS), screen.theme().bodyText);
    return;
  }

  // rowItems is built in loadRecentBooks() (see rebuildRowItems()) and
  // reused here on every repaint.
  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  // Tap opens; long-press prompts removal (physical buttons stay in loop()).
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  // Titles in the small font so more of a long title fits on the line; the row
  // height stays on the theme cadence. Bold keeps the title/author hierarchy
  // and doubles as the caller-owned marker: an all-default smallText fails
  // textStyleUnset and Screen::list() would substitute bodyText back
  // (FONT_SLOT_SMALL is 0). No maxLines=2 here: on subtitle rows the label
  // band is one line tall and a wrapped title would collide with the author.
  fui::TextStyle label = screen.theme().smallText;
  label.bold = true;
  props.labelText = label;
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

void RecentBooksActivity::drawFooter() {
  // No rows: blank the row-action hints, same as FileBrowserActivity.
  const bool empty = recentBooks.empty();
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), empty ? "" : tr(STR_OPEN), empty ? "" : tr(STR_DIR_UP),
                                            empty ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
