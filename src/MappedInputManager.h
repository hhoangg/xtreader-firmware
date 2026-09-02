#pragma once

#include <HalGPIO.h>

class GfxRenderer;
namespace freeink {
namespace ui {
enum class ScreenEdge : uint8_t;
}
}  // namespace freeink

class MappedInputManager {
 public:
  enum class Button {
    Back,
    Confirm,
    Left,
    Right,
    Up,
    Down,
    Power,
    PageBack,
    PageForward,
    NavNext,
    NavPrevious,
    ScreenLeft,
    ScreenRight,
    ScreenUp,
    ScreenDown
  };
  enum class SwipeDir { None, Left, Right, Up, Down };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  MappedInputManager(HalGPIO& gpio, const GfxRenderer& renderer) : gpio(gpio), renderer(renderer) {}

  // Frame boundary: advances the real GPIO edge state, and (test builds
  // only) clears the injected press/release edges so they read true for
  // exactly one frame no matter how many times a caller queries them within
  // it -- see the injected* fields below. Must be called exactly once per
  // loop() iteration, same as gpio.update() itself; src/main.cpp's loop()
  // calls this (not gpio.update() directly) for that reason.
  void update() const;
#if FREEINK_CAP_TOUCH
  // X4 Pro delays a single power click until its frontlight double-click window
  // expires. The main loop supplies that one-frame event here.
  void setPowerConfirmClickFrame(const bool clicked) { powerConfirmClickFrame = clicked; }
#endif
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  // One-shot threshold event while the button is down; consumes its release.
  bool wasLongPressed(Button button, unsigned long thresholdMs) const;
  bool consumeSuppressedRelease() const;
  bool isPressed(Button button) const;
#ifdef CP_TEST_CONSOLE
  // Test-console synthetic input injection, consulted by wasPressed() (and,
  // for holds, isPressed()/wasReleased()/getHeldTime()) before falling
  // through to the real GPIO path. Queues at most one pending button action;
  // enqueuing overwrites any not-yet-consumed one, same as a single physical
  // button never queuing two presses. holdMs = 0 models an instantaneous tap
  // (the release edge fires on the very next consult); holdMs > 0 keeps
  // isPressed() true for that long before the release edge fires, so
  // long-press flows gated on wasReleased()+getHeldTime() (see
  // FileBrowserActivity's delete-on-hold) see a real hold.
  //
  // Edge semantics match a real HalGPIO edge, not "first reader wins": a
  // physical release stays readable by every caller for the whole frame and
  // is cleared at the frame boundary by gpio.update() (see update() above),
  // not by whichever code happens to query it first. Several activities
  // query the same button more than once in one loop() pass under different
  // conditions (FileBrowserActivity's delete-on-hold check, then its plain
  // Back-to-parent check, is one; it is not the only one) -- a naive
  // "consume on first read" injection made the first, failing check eat the
  // edge and the second, real check see nothing. wasPressed()/wasReleased()/
  // isPressed()/getHeldTime() below are therefore idempotent within a frame:
  // update() is what clears them, not the read itself.
  void injectPress(Button button, unsigned long holdMs = 0);
#endif
  bool hasTouch() const;
  bool wasScreenTapped(int& x, int& y) const;
  bool wasScreenTouchDown(int& x, int& y) const;
  // One-shot long-press from the SDK touch classifier, fired WHILE the finger
  // is still down (stationary contact held past the SDK threshold). Consuming
  // it suppresses the remainder of the contact — its continued hold and its
  // release edge — so the ensuing finger lift can't also tap-dismiss the popup
  // the long-press opened. The SDK owns that latch and self-clears it once the
  // contact ends.
  bool wasScreenLongPress(int& x, int& y) const;
  bool isScreenTouchHeld(int& x, int& y) const;
  // Raw release edge, also true when the contact ended in a swipe or drag-off
  // (which wasScreenTapped never reports). InputSnapshot builders forward it
  // off-target so FreeInkUI routing clears its pressed-element state.
  bool wasScreenTouchReleased() const;
  bool wasTapInRect(int x, int y, int width, int height) const;

  // Combined touch interaction for a band of equal rows with caller-supplied
  // geometry — the shared hit-test for lists the theme helpers above do not
  // cover (custom row heights, option prompts, menus). Down = a held
  // tap-candidate is on a row (update the selection highlight); Tap = a tap
  // released on one (activate). rowHeight limits the hit to the top rowHeight
  // px of each step (0 = the full step, no gap band).
  enum class RowTouch : uint8_t { None, Down, Tap };
  RowTouch rowTouch(int& row, int top, int rowStep, int rowCount, int xStart = 0, int xEnd = INT32_MAX,
                    int rowHeight = 0) const;
  // Horizontal variant for side-by-side button pairs (confirmation prompts).
  RowTouch colTouch(int& col, int left, int colStep, int colCount, int yStart, int yEnd, int colWidth = 0) const;

  SwipeDir wasSwipe() const;
  // Back = left-to-right swipe anchored at the left edge. Public so swipe-mode
  // page turns (reader) can exclude it from a plain SwipeDir::Right.
  bool wasBackGesture() const;
  // Home-key boards use a short Home-key tap to exit; their bottom-edge swipe
  // is intentionally unused. Other boards retain the bottom-edge Home gesture.
  // The reader menu remains on its existing top-edge gesture and middle tap.
  bool wasHomeGesture() const;
  // A Home-key hold runs the configured long-press action in the reader.
  bool wasHomeKeyHold() const;
  bool wasMenuGesture() const;
  // Bottom-edge up-swipe as the reader-menu gesture (SHOW_READER_MENU's Swipe
  // Up option). Only meaningful on home-key boards, where Home lives on the
  // key and the bottom edge is free; elsewhere the same swipe is the Home
  // gesture and this returns false.
  bool wasReaderMenuSwipeUp() const;
  // Top-edge down-swipe opens the light panel when the active board actually
  // has a frontlight. ActivityManager consumes it before activity input.
  bool wasLightPanelGesture() const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  const GfxRenderer& getRenderer() const { return renderer; }
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Maps four screen-direction labels onto the two physical front-button roles
  // using the same live-orientation transform as ScreenLeft/Right/Up/Down.
  Labels mapDirectionalLabels(const char* back, const char* confirm, const char* left, const char* right,
                              const char* up, const char* down) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

  // True when the control axis is flipped relative to the physical buttons: always on touch boards,
  // or when button-only boards opt in, while the screen is currently INVERTED / LANDSCAPE_CCW.
  [[nodiscard]] bool isNavDirectionSwapped() const;

 private:
  HalGPIO& gpio;
  // Logical-to-physical button mapping depends on what the user is actually looking at: when the
  // screen is rendered rotated, the directional buttons must flip to match. The renderer is the only
  // authority on the *live* orientation (the reader rotates it and restores portrait on exit), so we
  // read it here instead of CrossPointSettings.orientation, which is just the persisted reader
  // preference and stays "rotated" even while portrait UI like home/settings is on screen.
  const GfxRenderer& renderer;

  Button mapScreenDirection(Button button) const;
  Labels mapFrontLabels(const char* back, const char* confirm, const char* left, const char* right) const;
  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  // SDK edge classification (fui::edgeSwipe) + the shared decode/held-time
  // bookkeeping; the wrappers below give each edge its board meaning.
  bool wasEdgeSwipe(freeink::ui::ScreenEdge edge) const;
  bool wasTopEdgeDownSwipe() const;
  bool wasBottomEdgeUpSwipe() const;
  // Fetch the pending swipe (if any) and map both endpoints to logical screen coords
  bool decodeSwipe(int& sx, int& sy, int& ex, int& ey) const;
#if FREEINK_CAP_TOUCH
  bool wasPowerConfirmClick() const;
#endif
  void rememberTouchHeldTime() const;
  void suppressNextRelease(Button button) const;

  mutable bool touchHeldOverrideValid = false;
  mutable unsigned long touchHeldOverrideMs = 0;
  mutable unsigned long touchHeldOverrideAt = 0;
  mutable uint16_t longPressFiredButtons = 0;
  mutable uint16_t suppressedReleaseButtons = 0;
#if FREEINK_CAP_TOUCH
  bool powerConfirmClickFrame = false;
#endif
#ifdef CP_TEST_CONSOLE
  // injectedPressPending: the press edge; true for exactly one frame (the
  // one injectPress() was called on), cleared by update() -- not by
  // wasPressed() reading it.
  mutable bool injectedPressPending = false;
  // injectedHeld: "still down, release edge not fired yet"; spans every
  // frame from injectPress() up to (and not including) the frame the
  // release deadline is crossed. Drives isPressed() and gates the deadline
  // check in wasReleased().
  mutable bool injectedHeld = false;
  // injectedReleaseEdge: the release edge; latched true the first time
  // wasReleased() notices the deadline has passed, stays true for every
  // query for the rest of that same frame, cleared by update().
  mutable bool injectedReleaseEdge = false;
  // Mirrors injectedReleaseEdge's one-frame lifetime for getHeldTime()'s
  // override value.
  mutable bool injectedHeldOverrideValid = false;
  mutable Button injectedButton = Button::Back;
  mutable unsigned long injectedReleaseAt = 0;
  mutable unsigned long injectedHeldMs = 0;
#endif
};
