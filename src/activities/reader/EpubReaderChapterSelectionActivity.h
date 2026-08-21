#pragma once
#include <Epub.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

class EpubReaderChapterSelectionActivity final : public UiListActivity {
  std::shared_ptr<Epub> epub;
  int currentSpineIndex = 0;

  // Only the rows currently on screen are materialised.
  //
  // These used to hold one entry per TOC item, built once in onEnter(). A
  // std::string plus a ListItem runs to roughly 100 bytes an entry, so a book
  // with a few thousand chapters wanted hundreds of KB -- against the ~50 KB of
  // heap a reading session leaves. Worse, the build is -fno-exceptions, so the
  // failed reserve() did not return null: it called std::terminate() and
  // rebooted the device the moment the chapter list was opened.
  //
  // The list widget is virtualised and only ever dereferences the rows it can
  // fit (plus one partial trailing row), so buildScreen() fills these with just
  // that slice, re-derived on every repaint. Cost is now constant in the size of
  // the book. Each row still carries its absolute TOC index in
  // ListItem::actionValue, so taps and the selection stay in absolute terms.
  std::vector<std::string> windowLabels;
  std::vector<freeink::ui::ListItem> windowItems;
  // Absolute TOC index that windowItems[0] corresponds to.
  int windowStart = 0;
  void buildWindow(int start, int count);

  // Total TOC items count
  int listCount() const override { return epub ? epub->getTocItemsCount() : 0; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Back cancels with a result and Confirm activates on RELEASE here, and a
  // missing epub swallows everything past Back.
  bool handleButtons() override;
  // Header is drawn inside the safe area (not full-width like the base).
  void drawChrome() override;

 public:
  explicit EpubReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const std::shared_ptr<Epub>& epub, int currentSpineIndex);
  void onEnter() override;
};
