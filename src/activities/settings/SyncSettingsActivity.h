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
  //
  // A 6th row, added by this task: Sync Now (sync_manifest::sync(), the
  // explicit counterpart to HomeActivity's automatic once-per-boot trigger --
  // see SyncTriggerPolicy.h). Appended at the end, after the fixed rows
  // above, rather than inserted among them: scripts/device_tests/test_pairing.py
  // reads this hub's rows by a fixed leading count (HUB_ROW_COUNT) to find
  // the Pair/Unlink action at index 2, and inserting a row earlier would
  // shift that index.
  static constexpr int MENU_ITEMS = 6;
#ifdef CP_TEST_CONSOLE
  bool getSelectedRowInfo(std::string& outLabel, int& outIndex, int& outCount) const override;
#endif

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  // Row storage: MENU_ITEMS is a compile-time constant, so fixed-capacity
  // storage avoids any heap allocation for the row list. Labels for rows
  // whose text is static are set
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

  // Same transient-status pattern as requestBooksStatus_ above, for the Sync
  // Now row.
  std::string syncNowStatus_;
  void syncNowTapped();
  void doManifestSync();
};
