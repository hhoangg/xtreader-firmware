#pragma once
#include <I18n.h>

#include <string>

#include "activities/UiTabListActivity.h"

/**
 * The fork's own features, in one place: device pairing, library sync and the
 * wallpaper gallery. Everything here used to be reached through upstream's
 * Settings screen, which meant a fork hunk in SettingsActivity per feature;
 * this screen owns them instead, so no hunk in SettingsActivity relates to a
 * fork feature this screen owns. (SettingsActivity.{cpp,h} still carry two
 * unrelated fork hunks of their own: the deletions that drop upstream's OPDS
 * browser and KOReader-sync screens, and the CP_TEST_CONSOLE-guarded
 * getSelectedRowInfo() accessor the device tests drive.)
 *
 * Absorbs the whole of the former SyncSettingsActivity -- its six rows are the
 * Account tab's three plus the Library tab's first three, in their original
 * relative order.
 */
class XtreaderActivity final : public UiTabListActivity {
 public:
  explicit XtreaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;

  // Stable ids, not positions: the row a case acts on must not change meaning
  // when a tab gains or loses an entry.
  enum RowId : int {
    ROW_SERVER_URL = 0,
    ROW_STATUS,
    ROW_PAIR_ACTION,
    ROW_REQUEST_BOOKS,
    ROW_QUEUE,
    ROW_SYNC_NOW,
    ROW_DOC_MATCHING,
    ROW_SEND_METADATA,
    ROW_WALLPAPER_GALLERY,
    ROW_SYNCED_ONLY,
    ROW_COUNT,
  };

  static constexpr int TAB_COUNT = 3;
  // The Library tab, the widest. Sizes the row storage below so no tab switch
  // allocates.
  static constexpr int MAX_TAB_ROWS = 5;

#ifdef CP_TEST_CONSOLE
  bool getSelectedRowInfo(std::string& outLabel, int& outIndex, int& outCount) const override;
#endif

 private:
  int activeTab_ = 0;

  // Rebuilt by rebuildRowItems() on entry and on every tab switch; only the
  // live value text is refreshed per repaint, by assigning into the existing
  // strings rather than reallocating.
  std::string rowValues_[MAX_TAB_ROWS];
  freeink::ui::ListItem rowItems_[MAX_TAB_ROWS]{};

  // Transient row feedback, cleared by leaving and re-entering the screen.
  // Same pattern the former SyncSettingsActivity used.
  std::string requestBooksStatus_;
  std::string syncNowStatus_;

  // --- UiTabListActivity contract ---
  int listCount() const override;
  int tabCount() const override { return TAB_COUNT; }
  int activeTab() const override { return activeTab_; }
  const char* tabLabel(int index) const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;
  bool handleButtons() override;
  const char* headerTitle() const override { return tr(STR_XTREADER); }
  void drawFooter() override;

  void selectTab(int index);
  void rebuildRowItems();
  RowId rowIdAt(int position) const;
  // The row dispatch itself, with no focus side effects. activateIndex() is the
  // touch path and adds the tap-first focus reset on top; handleButtons() is
  // the button path and must NOT, or a Confirm press on a touch device would
  // snap the cursor off the row and onto the tab band.
  void activateRow(int position);

  // Row handlers, moved verbatim from SyncSettingsActivity.
  void launchPairing();
  void unlinkDevice();
  void requestBooksTapped();
  void doRequestBooks();
  void toggleQueueCancel();
  void syncNowTapped();
  void doManifestSync();
  void editServerUrl();

  // New rows.
  void openWallpaperGallery();
  void cycleDocumentMatching();
  void cycleSendMetadata();
  void toggleSyncedWallpapersOnly();
};
