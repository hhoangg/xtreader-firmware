#pragma once
#include <I18n.h>

#include <functional>
#include <string>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "fontIds.h"

class ConfirmationActivity : public Activity {
 public:
  static constexpr int MAX_OPTIONS = 3;

 private:
  // Input data
  std::string heading;
  std::string body;

  const int margin = 20;
  const int spacing = 30;
  const int fontId = UI_10_FONT_ID;

  std::string safeHeading;
  std::string safeBody;
  OptionPopup confirmPopup;
  int startY = 0;
  int lineHeight = 0;

  // Option labels shown on the popup, in order; option 0 is always Cancel by
  // convention. Defaults to the original plain Cancel/Confirm pair -- every
  // existing 2-arg caller is unaffected.
  StrId optionLabels[MAX_OPTIONS] = {StrId::STR_CANCEL, StrId::STR_CONFIRM};
  int optionCount = 2;

 public:
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body);

  // 3-way variant: e.g. {STR_CANCEL, STR_DELETE_FROM_DEVICE, STR_DELETE_EVERYWHERE}
  // for FileBrowserActivity's force-delete option, whose two destructive
  // choices differ enough (local-only vs. account-wide) that a plain
  // "Delete?"/"Confirm" pair would not say which one is about to happen.
  // `optionLabels`/`optionCount` must outlive the call (copied into the
  // fixed member array immediately); optionCount must be in [2, MAX_OPTIONS].
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body, const StrId* optionLabels, int optionCount);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
};