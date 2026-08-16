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

  static constexpr int MENU_ITEMS = 3;
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
};
