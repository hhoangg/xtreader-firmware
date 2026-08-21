#include "EpubReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

EpubReaderChapterSelectionActivity::EpubReaderChapterSelectionActivity(GfxRenderer& renderer,
                                                                       MappedInputManager& mappedInput,
                                                                       const std::shared_ptr<Epub>& epub,
                                                                       const int currentSpineIndex)
    : UiListActivity("EpubReaderChapterSelection", renderer, mappedInput),
      epub(epub),
      currentSpineIndex(currentSpineIndex) {}

void EpubReaderChapterSelectionActivity::onEnter() {
  UiListActivity::onEnter();

  if (!epub) {
    return;
  }

  // Start with the current chapter at the top of the viewport; the first
  // screen build pulls the viewport to it (ListNav follow-on-build).
  int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (tocIndex == -1) {
    tocIndex = 0;
  }
  nav.selected = tocIndex;
}

// Materialises TOC entries [start, start + count) into the row buffers. Called
// from buildScreen() on every repaint with just the slice the list can show, so
// the buffers stay a fixed handful of rows however long the TOC is.
//
// getTocItem() reads from book.bin on the SD card, so this trades a bounded
// number of small reads per repaint for not holding the whole TOC in RAM.
void EpubReaderChapterSelectionActivity::buildWindow(const int start, const int count) {
  windowStart = start;
  windowLabels.clear();
  windowItems.clear();
  if (count <= 0) {
    return;
  }
  windowLabels.reserve(count);
  windowItems.reserve(count);
  for (int i = 0; i < count; i++) {
    const int absolute = start + i;
    const auto tocItem = epub->getTocItem(absolute);
    std::string indent(tocItem.level > 0 ? (tocItem.level - 1) * 2 : 0, ' ');
    windowLabels.push_back(indent + tocItem.title);
    fui::ListItem item;
    item.label = windowLabels.back().c_str();
    // Absolute index: onRowAction feeds this straight back to activateIndex().
    item.actionValue = static_cast<int16_t>(absolute);
    windowItems.push_back(item);
  }
}

void EpubReaderChapterSelectionActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) {
    return;
  }
  // The activated row leaves this screen (finish); a lingering flash would gray
  // an unrelated element on the next render.
  app.clearTapFlash();
  nav.selected = index;
  const auto tocItem = epub->getTocItem(index);
  if (tocItem.spineIndex == -1) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  } else {
    setResult(ChapterResult{tocItem.spineIndex, tocItem.anchor});
    finish();
  }
}

bool EpubReaderChapterSelectionActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }

  if (!epub) {
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateIndex(nav.selected);
    return true;
  }

  return false;
}

void EpubReaderChapterSelectionActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band drawChrome paints the title in.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (!epub) {
    return;
  }
  const int totalItems = listCount();
  if (totalItems <= 0) {
    screen.centeredText(tr(STR_NO_CHAPTERS), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  // Measure the band and clamp the viewport against the ABSOLUTE count first.
  // syncListViewport only writes nav.visibleRows, props.topIndex and
  // props.selectedIndex, and never reads props.items, so the window can be cut
  // from its results afterwards.
  syncListViewport(screen, props);

  // + 2 covers the partial trailing row the widget draws past the last row that
  // fully fits (it reads items[topIndex + visibleRows]).
  const int rows = nav.visibleRows > 0 ? nav.visibleRows : 1;
  const int start = props.topIndex;
  int count = rows + 2;
  if (start + count > totalItems) {
    count = totalItems - start;
  }
  buildWindow(start, count);
  if (windowItems.empty()) {
    return;
  }

  props.items = windowItems.data();
  props.count = static_cast<uint16_t>(windowItems.size());
  // The widget indexes items[] from topIndex upwards, so the slice has to be
  // rebased to start at 0. Row identity survives in ListItem::actionValue.
  props.topIndex = 0;
  const int selected = nav.selected - windowStart;
  props.selectedIndex =
      (selected >= 0 && selected < static_cast<int>(windowItems.size())) ? static_cast<int16_t>(selected) : -1;
  // The built-in indicator sizes its thumb from props.count/topIndex, which now
  // describe the window rather than the whole TOC. drawChrome() shows the
  // position as "n/total" instead.
  props.scrollIndicator = false;
  screen.list(props);
}

void EpubReaderChapterSelectionActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const char* title = tr(STR_SELECT_CHAPTER);
  // buildScreen() turns the scroll indicator off once the list is windowed, so
  // carry the position in the header whenever the TOC outruns one screen.
  char titleWithPosition[96];
  const int totalItems = listCount();
  if (totalItems > nav.visibleRows) {
    snprintf(titleWithPosition, sizeof(titleWithPosition), "%s  %d/%d", title, nav.selected + 1, totalItems);
    title = titleWithPosition;
  }
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight}, title);
}
