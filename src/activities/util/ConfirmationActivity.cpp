#include "ConfirmationActivity.h"

#include <I18n.h>

#include "HalDisplay.h"
#include "components/UITheme.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body)
    : Activity("Confirmation", renderer, mappedInput), heading(heading), body(body) {}

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body,
                                           const StrId* optionLabelsIn, const int optionCountIn)
    : Activity("Confirmation", renderer, mappedInput), heading(heading), body(body) {
  optionCount = (optionCountIn < 2) ? 2 : (optionCountIn > MAX_OPTIONS ? MAX_OPTIONS : optionCountIn);
  for (int i = 0; i < optionCount; i++) {
    optionLabels[i] = optionLabelsIn[i];
  }
}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  lineHeight = renderer.getLineHeight(fontId);
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);

  if (!heading.empty()) {
    safeHeading = renderer.truncatedText(fontId, heading.c_str(), maxWidth, EpdFontFamily::BOLD);
  }
  if (!body.empty()) {
    bodyLines = renderer.wrappedText(fontId, body.c_str(), maxWidth, MAX_BODY_LINES, EpdFontFamily::REGULAR);
  }

  // Text sits in the upper part of the screen so the confirmation popup
  // (centered) doesn't cover it.
  startY = renderer.getScreenHeight() / 6;

  const char* options[MAX_OPTIONS];
  for (int i = 0; i < optionCount; i++) {
    options[i] = I18N.get(optionLabels[i]);
  }
  // No caption inside the popup: the heading is already drawn above it at the
  // full screen width, where it fits. Repeating it in the dialog only got it
  // truncated a second time against the narrower popup.
  confirmPopup.show("", options, optionCount, 0, [this](int idx) {
    ActivityResult res{ConfirmationResult{idx}};
    // Index 0 is always Cancel by convention (see the header); this keeps
    // isCancelled meaningful for every existing 2-arg caller that only
    // checks that field, unchanged from before the 3-option variant existed.
    res.isCancelled = (idx == 0);
    setResult(std::move(res));
    finish();
  });

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  int currentY = startY;
  LOG_DBG("CONF", "currentY: %d", currentY);
  // Draw Heading
  if (!safeHeading.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeHeading.c_str(), true, EpdFontFamily::BOLD);
    currentY += lineHeight + spacing;
  }

  // Draw Body
  for (const auto& line : bodyLines) {
    renderer.drawCenteredText(fontId, currentY, line.c_str(), true, EpdFontFamily::REGULAR);
    currentY += lineHeight;
  }

  if (confirmPopup.processRender(renderer, mappedInput)) return;

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  // Popup dismissed without a selection (Back button or tap outside): cancel.
  ActivityResult res;
  res.isCancelled = true;
  setResult(std::move(res));
  finish();
}
