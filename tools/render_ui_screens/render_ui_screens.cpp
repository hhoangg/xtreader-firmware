// Host-side renderer for CrossPoint's FreeInkUI list screens.
//
// Renders the Account Sync settings list and two File Browser states
// (placeholder / queued rows) to BMP files, at the X4's real panel geometry
// (BoardConfig::XTEINK_X4), using the real device UI font (Ubuntu 10pt/12pt,
// the same EpdFont/EpdFontFamily glyph data src/main.cpp registers as
// UI_10_FONT_ID / UI_12_FONT_ID) and the real freeink::ui::list()/Screen
// component code from freeink-sdk. See the tool's README section at the
// bottom of this file for exactly what is shared production code vs.
// reproduced constants.
//
// Build:
//   cmake -S tools/render_ui_screens -B build/render_ui_screens
//   cmake --build build/render_ui_screens
// Run:
//   ./build/render_ui_screens/render_ui_screens <output_dir>

#include <BoardConfig.h>
#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <FreeInkApp.h>
#include <FreeInkUI.h>
#include <FreeInkUIIcon.h>
#include <I18n.h>
#include <Utf8.h>
#include <builtinFonts/ubuntu_10_bold.h>
#include <builtinFonts/ubuntu_10_regular.h>
#include <builtinFonts/ubuntu_12_bold.h>
#include <builtinFonts/ubuntu_12_regular.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "components/icons/listIcons.h"
#include "util/StringUtils.h"

namespace fui = freeink::ui;

// ============================================================================
// Real device fonts -- the same EpdFont/EpdFontFamily objects and glyph data
// src/main.cpp registers as UI_10_FONT_ID / UI_12_FONT_ID / SMALL_FONT_ID.
// This is the exact production font data (Ubuntu 10/12pt, built from
// Ubuntu-Vietnamese-Regular.ttf per the generated header's fontconvert.py
// command), not a substitute.
// ============================================================================
namespace {
EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);
}  // namespace

// ============================================================================
// Minimal BMP writer (24-bit uncompressed) -- avoids adding a PNG dependency.
// ============================================================================
namespace {
void writeBmp(const std::string& path, int width, int height, const std::vector<uint8_t>& rgb) {
  const int rowSize = ((width * 3 + 3) / 4) * 4;  // rows padded to 4 bytes
  const int dataSize = rowSize * height;
  const int fileSize = 54 + dataSize;

  std::vector<uint8_t> header(54, 0);
  header[0] = 'B';
  header[1] = 'M';
  *reinterpret_cast<uint32_t*>(&header[2]) = static_cast<uint32_t>(fileSize);
  *reinterpret_cast<uint32_t*>(&header[10]) = 54;  // pixel data offset
  *reinterpret_cast<uint32_t*>(&header[14]) = 40;  // DIB header size
  *reinterpret_cast<int32_t*>(&header[18]) = width;
  *reinterpret_cast<int32_t*>(&header[22]) = height;  // positive = bottom-up
  *reinterpret_cast<uint16_t*>(&header[26]) = 1;      // planes
  *reinterpret_cast<uint16_t*>(&header[28]) = 24;     // bpp
  *reinterpret_cast<uint32_t*>(&header[34]) = static_cast<uint32_t>(dataSize);

  std::ofstream out(path, std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "failed to open %s\n", path.c_str());
    std::exit(1);
  }
  out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));

  std::vector<uint8_t> row(rowSize, 0xFF);
  // BMP rows are bottom-up.
  for (int y = height - 1; y >= 0; --y) {
    for (int x = 0; x < width; ++x) {
      const uint8_t* px = &rgb[(static_cast<size_t>(y) * width + x) * 3];
      // BMP is BGR.
      row[x * 3 + 0] = px[2];
      row[x * 3 + 1] = px[1];
      row[x * 3 + 2] = px[0];
    }
    out.write(reinterpret_cast<const char*>(row.data()), rowSize);
  }
}
}  // namespace

// ============================================================================
// HostGfxTarget: a freeink::ui::DrawTarget backed directly by the real
// EpdFontFamily/EpdFont device font objects above, instead of GfxRenderer.
// GfxRenderer itself can't be linked here (lib/hal/HalDisplay.h -> <Arduino.h>
// per CLAUDE.md's host-only constraint), so this reproduces the specific
// GfxRenderer algorithms this tool needs (getTextWidth, getLineHeight,
// drawText's glyph loop, truncatedText, wrappedText -- all copied/adapted
// from lib/GfxRenderer/GfxRenderer.cpp) while calling the *same* EpdFont
// glyph/kerning/ligature/combining-mark code GfxRenderer calls. Only the
// device-facing plumbing (bidi reordering, SD-card fallback fonts, the
// physical framebuffer) is skipped -- none of it applies to the LTR
// Vietnamese/English strings these screens render. Fill/stroke/line/
// triangle/bitmap are a fresh, simple implementation against a plain RGB
// buffer (modeled on FreeInkUIDisplayTarget.h's plot() pattern), since
// those are generic pixel primitives with no font dependency.
// ============================================================================
class HostGfxTarget final : public fui::DrawTarget {
 public:
  HostGfxTarget(int16_t width, int16_t height)
      : w_(width), h_(height), rgb_(static_cast<size_t>(width) * height * 3, 0xFF) {}

  void setFont(const fui::FontId slot, EpdFontFamily* family) {
    if (slot < 3) fonts_[slot] = family;
  }

  fui::DeviceContext deviceContext() const {
    fui::DeviceContext device;
    device.width = w_;
    device.height = h_;
    device.orientation = fui::Orientation::Portrait;
    device.touchOrientation = fui::touchOrientationFor(device.orientation);
    device.hasTouch = false;
    device.hasButtons = true;
    return device;
  }

  int16_t width() const { return w_; }
  int16_t height() const { return h_; }
  const std::vector<uint8_t>& rgb() const { return rgb_; }

  // --- measurement: real EpdFontFamily::getTextDimensions, exactly what
  // GfxRenderer::getTextWidth() calls after its own bidi/SD-fallback plumbing
  // (neither of which applies to LTR Vietnamese/English text). ---
  int widthOf(const fui::FontId slot, const char* text, const EpdFontFamily::Style style) const {
    if (!text || !*text) return 0;
    int w = 0, h = 0;
    fontFor(slot)->getTextDimensions(text, &w, &h, style);
    return w;
  }

  fui::Size measureText(const fui::FontId font, const char* text, const fui::TextStyle style) const override {
    return fui::Size{static_cast<int16_t>(widthOf(font, text, styleFor(style))), lineHeight(font)};
  }

  // GfxRenderer::getLineHeight always reads the REGULAR variant's advanceY,
  // regardless of the requested style -- mirrored here.
  int16_t lineHeight(const fui::FontId font) const override {
    return static_cast<int16_t>(fontFor(font)->getData(EpdFontFamily::REGULAR)->advanceY);
  }

  // --- truncatedText/wrappedText: copied from GfxRenderer.cpp (pure string
  // + width algorithms, no hardware dependency) with getTextWidth calls
  // redirected to widthOf() above. ---
  std::string truncatedText(const fui::FontId font, const char* text, const int maxWidth,
                            const EpdFontFamily::Style style) const {
    if (!text || maxWidth <= 0) return "";
    std::string item = text;
    const char* ellipsis = "\xe2\x80\xa6";
    if (widthOf(font, item.c_str(), style) <= maxWidth) return item;
    while (!item.empty() && widthOf(font, (item + ellipsis).c_str(), style) >= maxWidth) {
      utf8RemoveLastChar(item);
    }
    return item.empty() ? ellipsis : item + ellipsis;
  }

  // --- drawing: real glyph bitmaps, real kerning/ligatures/combining marks
  // via EpdFont/EpdFontFamily (the identical calls GfxRenderer::drawText
  // makes); only the pixel sink differs (plot() into an RGB buffer here
  // instead of GfxRenderer's panel framebuffer). Adapted from
  // GfxRenderer::drawText / renderCharImpl (is2Bit=false path only -- the
  // Ubuntu UI fonts used here are all 1bpp uncompressed). ---
  void drawRun(const fui::FontId slot, const char* text, int x, const int y, const bool ink,
               const EpdFontFamily::Style style) {
    EpdFontFamily* family = fontFor(slot);
    const int yPos = y + family->getData(EpdFontFamily::REGULAR)->ascender;
    int lastBaseX = x;
    int lastBaseLeft = 0, lastBaseWidth = 0, lastBaseTop = 0;
    int32_t prevAdvanceFP = 0;
    uint32_t prevCp = 0;
    uint32_t cp;
    const char* cursor = text;
    while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&cursor)))) {
      if (utf8IsCombiningMark(cp)) {
        const EpdGlyph* mark = family->getGlyph(cp, style);
        if (!mark) continue;
        const auto anchor = combiningMark::anchorFor(cp);
        const int raiseBy = combiningMark::raiseAboveBase(anchor, mark->top, mark->height, lastBaseTop);
        const int mx =
            combiningMark::anchorOver(anchor, lastBaseX, lastBaseLeft, lastBaseWidth, mark->left, mark->width);
        plotGlyph(*mark, family->getData(style), mx, yPos - raiseBy, ink);
        continue;
      }
      cp = family->applyLigatures(cp, cursor, style);
      if (prevCp != 0) {
        const int32_t kernFP = family->getKerning(prevCp, cp, style);
        lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);
      }
      const EpdGlyph* glyph = family->getGlyph(cp, style);
      lastBaseLeft = glyph ? glyph->left : 0;
      lastBaseWidth = glyph ? glyph->width : 0;
      lastBaseTop = glyph ? glyph->top : 0;
      prevAdvanceFP = glyph ? glyph->advanceX : 0;
      if (glyph) plotGlyph(*glyph, family->getData(style), lastBaseX, yPos, ink);
      prevCp = cp;
    }
  }

  // --- DrawTarget interface ---

  void text(const fui::Rect rect, const char* txt, const fui::TextStyle style) override {
    if (!txt || rect.empty()) return;
    const EpdFontFamily::Style epdStyle = styleFor(style);
    const int lh = lineHeight(style.font);
    const uint8_t maxLines = style.maxLines > 0 ? style.maxLines : 1;
    const bool ink = !style.inverted && style.color != fui::Color::White;

    const auto drawAligned = [&](const std::string& line, const int y) {
      int x = rect.x;
      if (style.align != fui::TextAlign::Left) {
        const int tw = widthOf(style.font, line.c_str(), epdStyle);
        x = style.align == fui::TextAlign::Center ? rect.x + (rect.width - tw) / 2 : rect.x + rect.width - tw;
        if (x < rect.x) x = rect.x;
      }
      drawRun(style.font, line.c_str(), x, y, ink, epdStyle);
    };

    if (widthOf(style.font, txt, epdStyle) <= rect.width) {
      drawAligned(txt, rect.y + std::max(0, (rect.height - lh) / 2));
      return;
    }
    if (maxLines == 1) {
      drawAligned(truncatedText(style.font, txt, rect.width, epdStyle), rect.y + std::max(0, (rect.height - lh) / 2));
      return;
    }
    const std::vector<std::string> lines = wrappedText(style.font, txt, rect.width, maxLines, epdStyle);
    const int blockH = static_cast<int>(lines.size()) * lh;
    int y = rect.y + std::max(0, (rect.height - blockH) / 2);
    for (const auto& line : lines) {
      drawAligned(line, y);
      y += lh;
    }
  }

  void fill(const fui::Rect rect, const fui::Paint paint, const uint8_t radius = 0,
            const uint8_t corners = fui::CornersAll) override {
    if (rect.empty() || paint.kind == fui::PaintKind::None) return;
    if (paint.kind == fui::PaintKind::Bitmap) {
      bitmap(rect, paint.bitmap.bitmap, paint.bitmap.mode, fui::Paint::solid(fui::Color::Black));
      return;
    }
    if (paint.color == fui::Color::Transparent) return;
    for (int16_t y = rect.y; y < rect.bottom(); ++y) {
      for (int16_t x = rect.x; x < rect.right(); ++x) {
        if (radius > 0 && !insideRounded(rect, radius, corners, x, y)) continue;
        plotColor(x, y, paint.color);
      }
    }
  }

  void stroke(const fui::Rect rect, const fui::Paint paint, const uint8_t width, const uint8_t radius = 0,
              const uint8_t corners = fui::CornersAll) override {
    if (rect.empty() || width == 0 || paint.kind == fui::PaintKind::None || paint.color == fui::Color::Transparent)
      return;
    const fui::Rect inner = rect.inset(fui::Insets{width, width, width, width});
    for (int16_t y = rect.y; y < rect.bottom(); ++y) {
      for (int16_t x = rect.x; x < rect.right(); ++x) {
        const bool onOuter = radius == 0 || insideRounded(rect, radius, corners, x, y);
        if (!onOuter) continue;
        const bool inHole =
            !inner.empty() && (radius == 0 ? inner.contains(x, y) : insideRounded(inner, radius, corners, x, y));
        if (!inHole) plotColor(x, y, paint.color);
      }
    }
  }

  void line(const fui::Point from, const fui::Point to, const uint8_t width, const fui::Paint paint) override {
    if (paint.kind == fui::PaintKind::None || paint.color == fui::Color::Transparent) return;
    int x0 = from.x, y0 = from.y;
    const int x1 = to.x, y1 = to.y;
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    const int half = width / 2;
    while (true) {
      for (int by = -half; by <= half; ++by)
        for (int bx = -half; bx <= half; ++bx)
          plotColor(static_cast<int16_t>(x0 + bx), static_cast<int16_t>(y0 + by), paint.color);
      if (x0 == x1 && y0 == y1) break;
      const int e2 = 2 * err;
      if (e2 >= dy) {
        err += dy;
        x0 += sx;
      }
      if (e2 <= dx) {
        err += dx;
        y0 += sy;
      }
    }
  }

  void triangle(const fui::Point a, const fui::Point b, const fui::Point c, const fui::Paint paint) override {
    if (paint.kind == fui::PaintKind::None || paint.color == fui::Color::Transparent) return;
    const int16_t minX = std::min({a.x, b.x, c.x}), maxX = std::max({a.x, b.x, c.x});
    const int16_t minY = std::min({a.y, b.y, c.y}), maxY = std::max({a.y, b.y, c.y});
    for (int16_t y = minY; y <= maxY; ++y)
      for (int16_t x = minX; x <= maxX; ++x)
        if (inTriangle(x, y, a, b, c)) plotColor(x, y, paint.color);
  }

  void bitmap(const fui::Rect rect, const fui::BitmapRef bmp, const fui::BitmapMode mode,
              const fui::Paint foreground = fui::Paint::solid(fui::Color::Black),
              const fui::Rotation rotation = fui::Rotation::None) override {
    if (!bmp || rect.empty()) return;
    const fui::Color color = foreground.kind == fui::PaintKind::None ? fui::Color::Black : foreground.color;
    if (color == fui::Color::Transparent) return;
    fui::forEachBitmapPixel(
        rect, bmp, mode, [&](const int16_t px, const int16_t py) { plotColor(px, py, color); }, rotation);
  }

 private:
  int16_t w_, h_;
  std::vector<uint8_t> rgb_;
  EpdFontFamily* fonts_[3] = {nullptr, nullptr, nullptr};

  EpdFontFamily* fontFor(const fui::FontId slot) const { return fonts_[slot < 3 ? slot : 1]; }
  static EpdFontFamily::Style styleFor(const fui::TextStyle& s) {
    return s.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  }

  std::vector<std::string> wrappedText(const fui::FontId font, const char* text, const int maxWidth, const int maxLines,
                                       const EpdFontFamily::Style style) const {
    std::vector<std::string> lines;
    if (!text || maxWidth <= 0 || maxLines <= 0) return lines;
    std::string remaining = text;
    std::string currentLine;
    while (!remaining.empty()) {
      if (static_cast<int>(lines.size()) == maxLines - 1) {
        std::string lastContent = currentLine.empty() ? remaining : currentLine + " " + remaining;
        lines.push_back(truncatedText(font, lastContent.c_str(), maxWidth, style));
        return lines;
      }
      size_t spacePos = remaining.find(' ');
      std::string word;
      if (spacePos == std::string::npos) {
        word = remaining;
        remaining.clear();
      } else {
        word = remaining.substr(0, spacePos);
        remaining.erase(0, spacePos + 1);
      }
      std::string testLine = currentLine.empty() ? word : currentLine + " " + word;
      if (widthOf(font, testLine.c_str(), style) <= maxWidth) {
        currentLine = testLine;
      } else if (!currentLine.empty()) {
        lines.push_back(currentLine);
        if (widthOf(font, word.c_str(), style) > maxWidth) {
          lines.push_back(truncatedText(font, word.c_str(), maxWidth, style));
          currentLine.clear();
          if (static_cast<int>(lines.size()) >= maxLines) return lines;
        } else {
          currentLine = word;
        }
      } else {
        lines.push_back(truncatedText(font, word.c_str(), maxWidth, style));
        return lines;
      }
    }
    if (!currentLine.empty() && static_cast<int>(lines.size()) < maxLines) lines.push_back(currentLine);
    return lines;
  }

  void plotGlyph(const EpdGlyph& glyph, const EpdFontData* data, const int cursorX, const int baseline,
                 const bool ink) {
    if (data->is2Bit) return;  // not used by the Ubuntu UI fonts this tool renders
    const uint8_t* bmp = &data->bitmap[glyph.dataOffset];
    int pixelPosition = 0;
    for (int gy = 0; gy < glyph.height; ++gy) {
      const int screenY = baseline - glyph.top + gy;
      for (int gx = 0; gx < glyph.width; ++gx, ++pixelPosition) {
        const int screenX = cursorX + glyph.left + gx;
        const uint8_t byte = bmp[pixelPosition >> 3];
        const uint8_t bitIndex = 7 - (pixelPosition & 7);
        if ((byte >> bitIndex) & 1)
          plotColor(static_cast<int16_t>(screenX), static_cast<int16_t>(screenY),
                    ink ? fui::Color::Black : fui::Color::White);
      }
    }
  }

  static uint8_t bayerAt(const int16_t x, const int16_t y) {
    static constexpr uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    return kBayer[y & 3][x & 3];
  }

  void plotColor(const int16_t x, const int16_t y, const fui::Color color) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_ || color == fui::Color::Transparent) return;
    bool black;
    switch (color) {
      case fui::Color::Black:
        black = true;
        break;
      case fui::Color::DarkGray:
        black = bayerAt(x, y) < 12;
        break;
      case fui::Color::LightGray:
        black = bayerAt(x, y) < 4;
        break;
      case fui::Color::White:
      default:
        black = false;
        break;
    }
    uint8_t* px = &rgb_[(static_cast<size_t>(y) * w_ + x) * 3];
    if (black) {
      px[0] = px[1] = px[2] = 0x00;
    } else if (color == fui::Color::White) {
      px[0] = px[1] = px[2] = 0xFF;
    }
    // "off" gray pixels leave the background as-is, matching the device dither.
  }

  static bool insideRounded(const fui::Rect rect, const uint8_t radius, const uint8_t corners, const int16_t x,
                            const int16_t y) {
    if (x < rect.x || y < rect.y || x >= rect.right() || y >= rect.bottom()) return false;
    int16_t r = radius;
    const int16_t halfW = static_cast<int16_t>(rect.width / 2);
    const int16_t halfH = static_cast<int16_t>(rect.height / 2);
    if (r > halfW) r = halfW;
    if (r > halfH) r = halfH;
    if (r <= 0) return true;
    const bool left = x < rect.x + r, right = x >= rect.right() - r;
    const bool top = y < rect.y + r, bottom = y >= rect.bottom() - r;
    int16_t cx = 0, cy = 0;
    bool inCorner = false;
    if (left && top && (corners & fui::CornerTopLeft)) {
      cx = rect.x + r;
      cy = rect.y + r;
      inCorner = true;
    } else if (right && top && (corners & fui::CornerTopRight)) {
      cx = rect.right() - 1 - r;
      cy = rect.y + r;
      inCorner = true;
    } else if (left && bottom && (corners & fui::CornerBottomLeft)) {
      cx = rect.x + r;
      cy = rect.bottom() - 1 - r;
      inCorner = true;
    } else if (right && bottom && (corners & fui::CornerBottomRight)) {
      cx = rect.right() - 1 - r;
      cy = rect.bottom() - 1 - r;
      inCorner = true;
    }
    if (!inCorner) return true;
    const int dxp = x - cx, dyp = y - cy;
    return dxp * dxp + dyp * dyp <= r * r;
  }

  static bool inTriangle(const int16_t px, const int16_t py, const fui::Point a, const fui::Point b,
                         const fui::Point c) {
    const auto edge = [](const int16_t px, const int16_t py, const fui::Point a, const fui::Point b) {
      return static_cast<int32_t>(px - b.x) * (a.y - b.y) - static_cast<int32_t>(a.x - b.x) * (py - b.y);
    };
    const int32_t d1 = edge(px, py, a, b), d2 = edge(px, py, b, c), d3 = edge(px, py, c, a);
    const bool hasNeg = d1 < 0 || d2 < 0 || d3 < 0;
    const bool hasPos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(hasNeg && hasPos);
  }
};

// ============================================================================
// Reproduced theme metrics -- NOT linked from src/components/themes/lyra/
// LyraTheme.h (that file, via UITheme.h/CrossPointSettings.h, pulls Arduino
// headers). LYRA is the app's default theme (CrossPointSettings.h:270,
// `uint8_t uiTheme = LYRA`), so this snapshot mirrors the numbers a fresh
// device actually renders with. These are plain data copied from
// src/components/themes/lyra/LyraTheme.h's LyraMetrics table; if that table
// changes, this snapshot silently drifts and needs re-syncing by hand.
// ============================================================================
namespace LyraMetricsSnapshot {
constexpr int16_t topPadding = 5;
constexpr int16_t headerHeight = 84;
constexpr int16_t verticalSpacing = 16;
// listWithSubtitleRowHeight (60 in LyraTheme.h) is intentionally not used
// here: both screens below call syncListViewport()'s real production
// default (hasSubtitle=false), same as FileBrowserActivity::buildScreen()
// does, so listRowHeight is the correct base for every row -- list()'s own
// per-row growth (see freeink-sdk's list.h) still taller-fits any row whose
// wrapped label or subtitle needs more room.
constexpr int16_t listRowHeight = 40;
constexpr int16_t listRowGap = 0;
constexpr uint8_t listRowRadius = 6;
constexpr int16_t listInset = 20;
constexpr int16_t listSidePadding = 8;
constexpr fui::SelectionStyle listSelectionStyle = fui::SelectionStyle::LightPill;
constexpr int16_t listScrollWidth = 4;
constexpr uint8_t listScrollSide = 0;
constexpr bool listTitleBold = false;
constexpr int16_t headerSidePadding = 18;
constexpr uint8_t headerUnderlineSize = 3;
constexpr fui::TextAlign headerTitleAlign = fui::TextAlign::Left;
constexpr int16_t buttonHintsHeight = 40;
}  // namespace LyraMetricsSnapshot

// Builds ThemeTokens the same way src/components/UIThemeTokens.h's
// uiThemeTokens() does: real SDK themeTokensForLineHeight() (linked
// directly, host-safe) as the base, then the LyraMetricsSnapshot values
// layered on top exactly like uiThemeTokens() layers ThemeMetrics on.
fui::ThemeTokens buildLyraTokens(const HostGfxTarget& target) {
  fui::ThemeTokens tokens = fui::themeTokensForLineHeight(target.lineHeight(1));  // FONT_BODY slot = 1
  tokens.listRowGap = LyraMetricsSnapshot::listRowGap;
  tokens.listRowRadius = LyraMetricsSnapshot::listRowRadius;
  tokens.listInset = LyraMetricsSnapshot::listInset;
  tokens.listSidePadding = LyraMetricsSnapshot::listSidePadding;
  tokens.listSelectionStyle = LyraMetricsSnapshot::listSelectionStyle;
  tokens.listScrollWidth = LyraMetricsSnapshot::listScrollWidth;
  tokens.listScrollSide = LyraMetricsSnapshot::listScrollSide;
  tokens.headerHeight = LyraMetricsSnapshot::headerHeight;
  tokens.headerSidePadding = LyraMetricsSnapshot::headerSidePadding;
  tokens.headerUnderline = LyraMetricsSnapshot::headerUnderlineSize;
  tokens.headerTitleAlign = LyraMetricsSnapshot::headerTitleAlign;
  tokens.bodyText.bold = LyraMetricsSnapshot::listTitleBold;
  return tokens;
}

namespace {
using UiScreen = fui::Screen<24>;

// Simplified chrome: a left-aligned title band + bottom rule (approximates
// GUI.drawHeader's layout without its battery-icon/theme-specific pixel art)
// and a plain centered footer hint row. The list BODY below is the real
// freeink::ui::list() / Screen::list() production code -- only this chrome
// band is a stand-in. See the report for the exact boundary.
void drawSimplifiedHeader(UiScreen& screen, const char* title) {
  const auto& theme = screen.theme();
  const fui::Rect band = screen.takeTop(static_cast<int16_t>(theme.headerHeight));
  fui::TextStyle style = theme.titleText;
  style.font = 2;  // FONT_TITLE slot
  screen.target().text(band.inset(fui::Insets{0, theme.headerSidePadding, 0, theme.headerSidePadding}), title, style);
  if (theme.headerUnderline > 0) {
    screen.target().fill(fui::Rect{band.x, static_cast<int16_t>(band.bottom() - theme.headerUnderline), band.width,
                                   theme.headerUnderline},
                         fui::Paint::solid(fui::Color::Black));
  }
}

void drawSimplifiedFooter(UiScreen& screen, const char* hint) {
  const auto& theme = screen.theme();
  const fui::Rect band = screen.takeBottom(LyraMetricsSnapshot::buttonHintsHeight);
  fui::TextStyle style = theme.smallText;
  style.align = fui::TextAlign::Center;
  screen.target().text(band, hint, style);
}

struct RenderedScreen {
  HostGfxTarget target;
  fui::ThemeTokens theme;
  explicit RenderedScreen(int16_t w, int16_t h) : target(w, h) {
    target.setFont(0, &ui10FontFamily);  // FONT_SMALL
    target.setFont(1, &ui12FontFamily);  // FONT_BODY
    target.setFont(2, &ui12FontFamily);  // FONT_TITLE
    theme = buildLyraTokens(target);
  }
};

void save(RenderedScreen& screen, const std::string& outDir, const char* name) {
  const std::string path = outDir + "/" + name;
  writeBmp(path, screen.target.width(), screen.target.height(), screen.target.rgb());
  std::fprintf(stderr, "wrote %s (%dx%d)\n", path.c_str(), screen.target.width(), screen.target.height());
}

// ---- Screen 1: Account Sync settings list -----------------------------
void renderSyncSettings(const std::string& outDir, const size_t serverUrlValueMaxChars) {
  RenderedScreen rs(static_cast<int16_t>(BoardConfig::XTEINK_X4.displayHeight),
                    static_cast<int16_t>(BoardConfig::XTEINK_X4.displayWidth));

  I18N.setLanguage(Language::EN);

  const std::string rawUrl = "crosspoint-sync.hoangxuan2402.workers.dev";
  const std::string serverUrlValue = StringUtils::middleEllipsis(rawUrl, serverUrlValueMaxChars);
  const std::string statusValue = "reader@example.com";           // paired state
  const std::string deviceNameValue = "Máy Đọc Sách Xuân Hoàng";  // Vietnamese device name, real diacritics

  fui::ListItem items[6];
  items[0] = fui::ListItem{
      tr(STR_CROSSPOINT_SYNC_SERVER_URL), nullptr, serverUrlValue.c_str(), {}, {}, fui::StateNormal, 0, true, false};
  items[1] =
      fui::ListItem{tr(STR_PAIRING_STATUS), nullptr, statusValue.c_str(), {}, {}, fui::StateNormal, 1, true, false};
  items[2] =
      fui::ListItem{tr(STR_UNLINK_DEVICE), nullptr, deviceNameValue.c_str(), {}, {}, fui::StateNormal, 2, true, false};
  items[3] = fui::ListItem{tr(STR_REQUEST_BOOKS), nullptr, nullptr, {}, {}, fui::StateNormal, 3, true, false};
  items[4] = fui::ListItem{tr(STR_DOWNLOAD_QUEUE), nullptr, nullptr, {}, {}, fui::StateNormal, 4, true, false};
  items[5] = fui::ListItem{tr(STR_SYNC_NOW), nullptr, nullptr, {}, {}, fui::StateNormal, 5, true, false};

  fui::InputSnapshot input{};
  fui::InteractionBuffer<24> interactions;
  // Frame stores a `const DeviceContext&` (it does not copy) -- keep the
  // context alive in a named local for the frame's lifetime, or it dangles
  // the moment the temporary from deviceContext() is destroyed.
  const fui::DeviceContext device = rs.target.deviceContext();
  fui::Frame<24> frame(rs.target, device, input, interactions);
  UiScreen screen(frame, rs.theme);

  drawSimplifiedHeader(screen, tr(STR_ACCOUNT_SYNC));

  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(LyraMetricsSnapshot::topPadding + LyraMetricsSnapshot::headerHeight), 0,
                  LyraMetricsSnapshot::buttonHintsHeight, 0});
  screen.spacer(LyraMetricsSnapshot::verticalSpacing);

  fui::ListProps props;
  props.items = items;
  props.count = 6;
  props.selectedIndex = -1;
  props.action = fui::NO_ACTION;
  props.valueInset = 8;
  // Non-touch hardware (X4 has no touch -- BoardConfig::XTEINK_X4.touch is
  // NO_TOUCH) keeps the dense per-theme row height instead of the
  // touch-target-sized theme default; see UiListActivity::syncListViewport.
  props.rowHeight = LyraMetricsSnapshot::listRowHeight;
  screen.list(props);

  drawSimplifiedFooter(screen, "Back        Select");

  save(rs, outDir, "sync-settings.bmp");
}

// ---- Screens 2 & 3: File Browser (placeholder / queued rows) ----------
// language defaults to Vietnamese (the task's target); the English render
// exists solely to eyeball STR_BOOK_DOWNLOADING="Downloading" (124px, wider
// than the Vietnamese "Đang tải") against real titles before keeping it.
void renderFileBrowser(const std::string& outDir, const char* fileName, const bool showQueued,
                       const Language language = Language::VI) {
  RenderedScreen rs(static_cast<int16_t>(BoardConfig::XTEINK_X4.displayHeight),
                    static_cast<int16_t>(BoardConfig::XTEINK_X4.displayWidth));

  I18N.setLanguage(language);

  // Row content: normal local books (title + extension) mixed with
  // server-only placeholder rows (title + download status), both in the
  // same value slot -- matching FileBrowserActivity::rebuildRowItems()'s
  // item construction after the value-slot fix (no more subtitle line: an
  // empty value used to be the only thing separating a placeholder row from
  // a normal one, which read as "no data yet" rather than "not on this
  // device").
  struct Row {
    std::string title;
    std::string value;
  };
  std::vector<Row> rows = {
      {"Truyện Kiều - Nguyễn Du.epub", "EPUB"},
      {"Đắc Nhân Tâm - Dale Carnegie.epub", "EPUB"},
      {"Những Người Khốn Khổ (Les Misérables) - Victor Hugo, Bản Dịch Đầy Đủ Không Rút Gọn.epub",
       tr(STR_BOOK_ON_SERVER)},
      {"Số Đỏ - Vũ Trọng Phụng.epub", showQueued ? tr(STR_BOOK_DOWNLOADING) : "EPUB"},
      {"Chí Phèo - Nam Cao.epub", "EPUB"},
      {"Dế Mèn Phiêu Lưu Ký - Tô Hoài.epub", tr(STR_BOOK_ON_SERVER)},
  };

  std::vector<fui::ListItem> items;
  items.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    fui::ListItem item;
    item.label = rows[i].title.c_str();
    if (!rows[i].value.empty()) item.value = rows[i].value.c_str();
    // Every row is a book (.epub) here -- same 24px icon size
    // FileBrowserActivity::rebuildRowItems() passes (listIconFor's default),
    // for both normal and placeholder rows.
    item.icon = fui::bitmapFromIcon(icon_book_24);
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::InputSnapshot input{};
  fui::InteractionBuffer<24> interactions;
  // Frame stores a `const DeviceContext&` (it does not copy) -- keep the
  // context alive in a named local for the frame's lifetime, or it dangles
  // the moment the temporary from deviceContext() is destroyed.
  const fui::DeviceContext device = rs.target.deviceContext();
  fui::Frame<24> frame(rs.target, device, input, interactions);
  UiScreen screen(frame, rs.theme);

  drawSimplifiedHeader(screen, "SD Card");

  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(LyraMetricsSnapshot::topPadding + LyraMetricsSnapshot::headerHeight), 0,
                  LyraMetricsSnapshot::buttonHintsHeight, 0});
  screen.spacer(LyraMetricsSnapshot::verticalSpacing);

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = -1;
  props.action = fui::NO_ACTION;
  props.valueInset = 8;
  // FileBrowserActivity::buildScreen: label uses theme.smallText with
  // maxLines=2 (file names wrap onto a second line within the row).
  fui::TextStyle label = rs.theme.smallText;
  label.maxLines = 2;
  props.labelText = label;
  props.balanceWrappedLabelWithValue = false;
  props.rowHeight = LyraMetricsSnapshot::listRowHeight;
  screen.list(props);

  drawSimplifiedFooter(screen, "Back        Open        Up        Down");

  save(rs, outDir, fileName);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s OUTPUT_DIR\n", argv[0]);
    return 2;
  }
  const std::string outDir = argv[1];

  // X4 panel geometry from BoardConfig, not hardcoded: the panel is
  // native-landscape 800x480; held-tall portrait swaps it to 480 wide,
  // 800 tall (matches DisplayTarget/GfxRenderer's Portrait rotation).
  const int16_t screenW = static_cast<int16_t>(BoardConfig::XTEINK_X4.displayHeight);
  const int16_t screenH = static_cast<int16_t>(BoardConfig::XTEINK_X4.displayWidth);

  int computedServerUrlMaxChars = 12;  // fallback if measurement finds nothing

  // --- Measurement pass: the numbers the report is built on. ---
  {
    HostGfxTarget probe(screenW, screenH);
    probe.setFont(0, &ui10FontFamily);
    probe.setFont(1, &ui12FontFamily);
    probe.setFont(2, &ui12FontFamily);
    I18N.setLanguage(Language::EN);

    const int labelW = probe.widthOf(1, tr(STR_CROSSPOINT_SYNC_SERVER_URL), EpdFontFamily::REGULAR);
    const std::string hostname = "crosspoint-sync.hoangxuan2402.workers.dev";

    // Row geometry: LyraTheme metrics over the portrait content band.
    const int listInset = LyraMetricsSnapshot::listInset;
    const int sidePad = LyraMetricsSnapshot::listSidePadding;
    const int rowAreaW = screenW - listInset * 2;  // no scroll indicator: 6 rows fit without overflow
    const int bandW = rowAreaW - sidePad * 2;
    const int valueInset = 8;
    const int textGap = 10;  // ListProps::textGap default

    std::fprintf(stderr,
                 "\n--- Server URL row measurement (Ubuntu 12pt regular label, Ubuntu 10pt regular value) ---\n");
    std::fprintf(stderr, "screen width (X4 portrait, from BoardConfig::XTEINK_X4): %d\n", screenW);
    std::fprintf(stderr, "row content band width (listInset=%d, listSidePadding=%d): %d\n", listInset, sidePad, bandW);
    std::fprintf(stderr, "label \"%s\" width: %d px\n", tr(STR_CROSSPOINT_SYNC_SERVER_URL), labelW);
    std::fprintf(stderr, "budget for value (band - label - valueInset(%d) - textGap(%d)): %d px\n", valueInset, textGap,
                 bandW - labelW - valueInset - textGap);

    const int valueBudget = bandW - labelW - valueInset - textGap;
    int maxChars = 0;
    for (size_t n = 1; n <= hostname.size(); ++n) {
      const std::string candidate = StringUtils::middleEllipsis(hostname, n);
      const int w = probe.widthOf(0, candidate.c_str(), EpdFontFamily::REGULAR);
      if (w <= valueBudget) maxChars = static_cast<int>(n);
    }
    std::fprintf(stderr, "hostname \"%s\" (%zu chars) full width: %d px\n", hostname.c_str(), hostname.size(),
                 probe.widthOf(0, hostname.c_str(), EpdFontFamily::REGULAR));
    std::fprintf(stderr, "max value chars that keep the full label visible: %d\n", maxChars);
    computedServerUrlMaxChars = maxChars;
    for (int n : {8, 12, maxChars, maxChars + 4}) {
      if (n <= 0) continue;
      const std::string v = StringUtils::middleEllipsis(hostname, static_cast<size_t>(n));
      const int w = probe.widthOf(0, v.c_str(), EpdFontFamily::REGULAR);
      std::fprintf(stderr, "  maxChars=%2d -> value=\"%s\" width=%dpx (fits label=%s)\n", n, v.c_str(), w,
                   (w <= valueBudget) ? "yes" : "NO");
    }

    // Download status now renders in the row's *value* slot (right-aligned,
    // same column a normal row's extension uses) instead of a subtitle line
    // -- see FileBrowserActivity::rebuildRowItems(). There is no hard pixel
    // cap on that slot (list()/GfxRendererTarget::text() never truncates
    // item.value; an over-wide value just eats into the label's space), so
    // "fits" here means "close to the width of EPUB", the reference for what
    // already reads as a short tag in that column, not a hard pass/fail line.
    // STR_BOOK_ON_SERVER/STR_BOOK_DOWNLOADING are dedicated to this column so
    // the shared STR_NOT_DOWNLOADED_YET/STR_DOWNLOADING (FontDownloadActivity
    // -- a full line to spare) can stay at their natural length; printed below
    // to confirm they were restored, not trimmed to fit this column.
    const int epubW = probe.widthOf(0, "EPUB", EpdFontFamily::REGULAR);
    std::fprintf(stderr, "\n--- File browser value-column width (Ubuntu 10pt regular; \"EPUB\" reference = %dpx) ---\n",
                 epubW);
    I18N.setLanguage(Language::EN);
    for (const char* s : {"Downloading", "Queued", "Not downloaded", "On server", "Cloud only", "Remote"}) {
      std::fprintf(stderr, "  EN \"%s\" width=%dpx\n", s, probe.widthOf(0, s, EpdFontFamily::REGULAR));
    }
    std::fprintf(stderr, "  EN column (dedicated keys): \"%s\" (%dpx) / \"%s\" (%dpx)\n", tr(STR_BOOK_DOWNLOADING),
                 probe.widthOf(0, tr(STR_BOOK_DOWNLOADING), EpdFontFamily::REGULAR), tr(STR_BOOK_ON_SERVER),
                 probe.widthOf(0, tr(STR_BOOK_ON_SERVER), EpdFontFamily::REGULAR));
    std::fprintf(stderr,
                 "  EN shared, unshortened (other screens): STR_DOWNLOADING=\"%s\" STR_NOT_DOWNLOADED_YET=\"%s\"\n",
                 tr(STR_DOWNLOADING), tr(STR_NOT_DOWNLOADED_YET));

    I18N.setLanguage(Language::VI);
    for (const char* s : {"Đang tải", "Đang tải về...", "Chưa tải", "Chưa tải về"}) {
      std::fprintf(stderr, "  VI \"%s\" width=%dpx\n", s, probe.widthOf(0, s, EpdFontFamily::REGULAR));
    }
    std::fprintf(stderr, "  VI column (dedicated keys): \"%s\" (%dpx) / \"%s\" (%dpx)\n", tr(STR_BOOK_DOWNLOADING),
                 probe.widthOf(0, tr(STR_BOOK_DOWNLOADING), EpdFontFamily::REGULAR), tr(STR_BOOK_ON_SERVER),
                 probe.widthOf(0, tr(STR_BOOK_ON_SERVER), EpdFontFamily::REGULAR));
    std::fprintf(stderr,
                 "  VI shared, unshortened (other screens): STR_DOWNLOADING=\"%s\" STR_NOT_DOWNLOADED_YET=\"%s\"\n",
                 tr(STR_DOWNLOADING), tr(STR_NOT_DOWNLOADED_YET));
  }

  // --- Render pass: the actual screens. ---
  // Deployed value: 2 chars of margin below the measured exact-fit ceiling
  // (computedServerUrlMaxChars) -- see SyncSettingsActivity.cpp's
  // SERVER_URL_VALUE_MAX_CHARS comment for why. Kept in sync with that
  // constant by hand; if one changes, update the other.
  const int deployedServerUrlMaxChars = computedServerUrlMaxChars - 2;
  std::fprintf(stderr, "\nmeasured ceiling = %d chars; rendering with the deployed SERVER_URL_VALUE_MAX_CHARS = %d\n",
               computedServerUrlMaxChars, deployedServerUrlMaxChars);
  renderSyncSettings(outDir, static_cast<size_t>(deployedServerUrlMaxChars));
  renderFileBrowser(outDir, "file-browser-placeholder.bmp", false);
  renderFileBrowser(outDir, "file-browser-queued.bmp", true);
  // English check only -- not one of the task's three deliverable screens;
  // exists to eyeball "Downloading" (124px) against real titles per the
  // coordinator's request before keeping it over "Queued" (75px).
  renderFileBrowser(outDir, "file-browser-queued-en-check.bmp", true, Language::EN);

  return 0;
}
