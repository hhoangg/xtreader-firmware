#include <gtest/gtest.h>

#include <array>

#include "components/icons/xtreader.h"

namespace {

constexpr int ICON_SIZE = 32;
constexpr int ROW_BYTES = (ICON_SIZE + 7) / 8;

// Mirrors drawNavIcon() in src/activities/home/HomeActivity.cpp: the asset is
// stored pre-rotated, and a CLEAR bit is ink. Screen pixel (dx, dy) reads the
// bit at row = ICON_SIZE - 1 - dx, col = dy.
bool isInk(const int dx, const int dy) {
  const int row = ICON_SIZE - 1 - dx;
  const int col = dy;
  const uint8_t byte = XtreaderIcon[row * ROW_BYTES + (col >> 3)];
  return ((byte >> (7 - (col & 7))) & 1) == 0;
}

int inkCount() {
  int count = 0;
  for (int dx = 0; dx < ICON_SIZE; dx++) {
    for (int dy = 0; dy < ICON_SIZE; dy++) {
      if (isInk(dx, dy)) count++;
    }
  }
  return count;
}

}  // namespace

TEST(XtreaderIcon, IsExactlyOne32x32Bitmap) {
  EXPECT_EQ(sizeof(XtreaderIcon), static_cast<size_t>(ICON_SIZE * ROW_BYTES));
}

// The whole point of the asset: the mark, not the logo's filled background.
// Every corner of the logo's rounded body is solid, so a corner with ink in it
// means logo.svg was converted instead of logo-mark.svg.
TEST(XtreaderIcon, CornersAreBlank) {
  EXPECT_FALSE(isInk(0, 0));
  EXPECT_FALSE(isInk(ICON_SIZE - 1, 0));
  EXPECT_FALSE(isInk(0, ICON_SIZE - 1));
  EXPECT_FALSE(isInk(ICON_SIZE - 1, ICON_SIZE - 1));
}

// Catches both failure modes a bad conversion produces: an empty bitmap (the
// SVG failed to rasterize and every bit stayed set) and a solid slab (the
// background was included, or the threshold inverted). The reference render of
// logo-mark.svg at 32x32 is ~25% ink, and the other four nav icons sit between
// 14% and 28%, so the band is wide enough that antialiasing differences between
// cairo versions cannot trip it.
TEST(XtreaderIcon, InkCoverageIsInRange) {
  const int ink = inkCount();
  const int total = ICON_SIZE * ICON_SIZE;
  EXPECT_GT(ink, total * 15 / 100) << "icon is blank or nearly blank";
  EXPECT_LT(ink, total * 40 / 100) << "icon is a solid block -- was logo.svg converted instead of logo-mark.svg?";
}

// The mark should fill most of the cell rather than sitting as a speck in one
// corner: both axes must span at least three quarters of the icon.
TEST(XtreaderIcon, MarkSpansMostOfTheIcon) {
  int minX = ICON_SIZE, maxX = -1, minY = ICON_SIZE, maxY = -1;
  for (int dx = 0; dx < ICON_SIZE; dx++) {
    for (int dy = 0; dy < ICON_SIZE; dy++) {
      if (!isInk(dx, dy)) continue;
      minX = std::min(minX, dx);
      maxX = std::max(maxX, dx);
      minY = std::min(minY, dy);
      maxY = std::max(maxY, dy);
    }
  }
  ASSERT_GE(maxX, 0) << "no ink at all";
  EXPECT_GE(maxX - minX + 1, ICON_SIZE * 3 / 4);
  EXPECT_GE(maxY - minY + 1, ICON_SIZE * 3 / 4);
}
