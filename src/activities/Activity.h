#pragma once
#include <Logging.h>

#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include "ActivityManager.h"  // for using the ActivityManager singleton
#include "ActivityResult.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "RenderLock.h"
#include "util/ScreenshotInfo.h"

class Activity {
  friend class ActivityManager;

 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

  ActivityResultHandler resultHandler;
  ActivityResult result;

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}
  virtual ~Activity() = default;
  virtual void onEnter();
  virtual void onExit();
  virtual void loop() {}

  virtual void render(RenderLock&&) {}

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  virtual void requestUpdate(bool immediate = false);

  // Request an immediate render and block until it completes.
  virtual void requestUpdateAndWait();

  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  virtual bool isReaderActivity() const { return false; }
  // True for the reading surfaces night mode inverts (EPUB/TXT/XTC). Resolved
  // per render by ActivityManager, so menus, overlays, and every other screen
  // keep normal polarity without managing the display flag themselves.
  virtual bool appliesNightMode() const { return false; }
  // Returns true when the activity schedules its own forced refresh.
  virtual bool handleForcedRefresh() { return false; }
  virtual bool isHomeActivity() const { return false; }
  virtual bool handleHomeGesture() { return false; }
  virtual ScreenshotInfo getScreenshotInfo() const { return {}; }
#ifdef CP_TEST_CONSOLE
  // Test-console introspection (CMD:ACTIVITY): the cheapest possible
  // assertion that navigation landed where it should.
  const std::string& getName() const { return name; }
  // Test-console introspection (CMD:SELECTED): reports whatever row/icon is
  // currently highlighted, so a host script can drive menu navigation by
  // reading real UI content instead of counting rows (a hardcoded row index
  // silently picks the wrong item once a conditional row -- e.g.
  // HomeActivity's OPDS Browser icon -- shifts everything after it; probing
  // by pressing CONFIRM and backing out is worse still, since it mutates
  // toggle settings in place and triggers real side effects like a Wi-Fi
  // scan on whatever it CONFIRMs along the way).
  //
  // Returns false when this activity doesn't support the introspection (not
  // a list/menu screen, or not wired up for it yet -- only the activities
  // scripts/device_tests/test_pairing.py actually drives implement this so
  // far). When it returns true, outIndex/outCount are always valid, but
  // outLabel may be empty if the current selection has no text label (e.g.
  // one of HomeActivity's recent-book cover tiles) -- callers should treat
  // that as "keep moving", not as an error.
  virtual bool getSelectedRowInfo(std::string& outLabel, int& outIndex, int& outCount) const { return false; }
#endif

  // Start a new activity without destroying the current one
  // Note: requestUpdate() will be invoked automatically once resultHandler finishes
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler);

  // Set the result to be passed back to the previous activity when this activity finishes
  void setResult(ActivityResult&& result);

  // Finish this activity and return to the previous one on the stack (if any)
  void finish();

  // Convenience method to facilitate API transition to ActivityManager
  // TODO: remove this in near future
  void onGoHome(HomeMenuItem item = HomeMenuItem::NONE);
  void onSelectBook(const std::string& path);
};
