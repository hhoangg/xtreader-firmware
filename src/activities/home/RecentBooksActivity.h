#pragma once
#include <HomeBookSlots.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/UiListActivity.h"
#include "sync/DownloadQueue.h"

class RecentBooksActivity final : public UiListActivity {
 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;

 private:
  int listCount() const override { return static_cast<int>(recentBooks.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  // Confirm activates on RELEASE here (a hold is "remove from list"), and Back
  // goes home rather than finishing.
  bool handleButtons() override;
  const char* headerTitle() const override { return tr(STR_MENU_RECENT_BOOKS); }
  void drawFooter() override;

  std::vector<RecentBook> recentBooks;
  // Row buffer, built in loadRecentBooks() (not buildScreen(), which reuses
  // it on every repaint instead of rebuilding a ListItem vector per render).
  std::vector<freeink::ui::ListItem> rowItems;
  // Status text for a remote row (e.g. "On server - 2.3 MB"), parallel to
  // recentBooks; empty (and unused) for a local row. rowItems' value points
  // into this, so it lives as long as rowItems does.
  std::vector<std::string> rowValues;
  void rebuildRowItems();

  // Data loading
  void loadRecentBooks();

  // Today's download_queue::snapshot(), reshaped into the form
  // home_book_slots::remoteState() reads -- the same conversion
  // HomeActivity::rebuildSlots() does, needed here too since this screen
  // shows the same remote entries with the same state wording.
  home_book_slots::QueueView buildQueueView() const;

  // A remote row (non-empty remoteId) was activated: enqueue it, same as
  // Home's OnServer/Failed row. Queued/Downloading is a no-op here too --
  // only download_queue::cancelAll() exists, there is no per-row cancel.
  void activateRemote(const RecentBook& book);

  // Same popup Home shows when enqueue() refuses a row, reusing its wording.
  void showEnqueueRefused(download_queue::EnqueueOutcome outcome);

  // Show an OK/Cancel prompt to remove the given book from the Recent Books list.
  void promptRemoveBook(const std::string& path, const std::string& title);
};
