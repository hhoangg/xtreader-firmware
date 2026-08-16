#pragma once

#include <string>

#include "activities/UiListActivity.h"

/**
 * Hub for CrossPoint Sync device pairing: server URL (self-hoster override),
 * pairing status, and the Pair/Re-pair/Unlink action. The actual RFC 8628
 * device-flow screens (QR code, polling, success/failure) live in
 * SyncPairingActivity, launched from here.
 */
class SyncSettingsActivity final : public UiListActivity {
 public:
  explicit SyncSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  // Two rows added by the book-download-engine task, minimal on purpose --
  // see the task's report for why they live here and not in a dedicated
  // screen:
  //  - Request Books: POST /feedback/request-books, the one-button "the
  //    library has run dry" signal (docs/API.md's "Feedback and telemetry").
  //  - Download Queue: read-only count of what download_queue.h currently
  //    has queued/downloading, doubling as the "get out" cancel action
  //    (download_queue::cancelAll()) -- the same single-row,
  //    state-dependent-label pattern ROW_PAIR_ACTION already uses below.
  static constexpr int MENU_ITEMS = 5;
#ifdef CP_TEST_CONSOLE
  bool getSelectedRowInfo(std::string& outLabel, int& outIndex, int& outCount) const override;
#endif

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  // Row storage: MENU_ITEMS is a compile-time constant, so fixed-capacity
  // storage avoids any heap allocation for the row list, matching
  // KOReaderSettingsActivity. Labels for rows whose text is static are set
  // once in the constructor; buildScreen() refreshes only the live value
  // text (rowValues_) and the two rows whose label itself changes with
  // pairing state (status row's value, and the pair/unlink action labels).
  std::string rowValues_[MENU_ITEMS];
  freeink::ui::ListItem rowItems_[MENU_ITEMS]{};

  void launchPairing();
  void unlinkDevice();

  // Result of the last Request Books tap, shown as the row's value until the
  // next tap or the next time this screen is left and re-entered -- purely
  // transient UI feedback, not persisted anywhere.
  std::string requestBooksStatus_;
  void requestBooksTapped();
  void doRequestBooks();
  void toggleQueueCancel();
};
