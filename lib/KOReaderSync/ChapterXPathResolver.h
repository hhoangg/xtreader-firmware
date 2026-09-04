#pragma once

#include <Epub.h>

#include <cstdint>
#include <memory>
#include <string>

class ChapterXPathResolver {
 public:
  /**
   * Resolve the Nth paragraph in a spine item to its real XHTML ancestry path.
   *
   * Returns a KOReader-compatible path like:
   * /body/DocFragment[8]/body/div[2]/section[1]/p[4]
   *
   * An empty string means parsing failed or the paragraph index was not found.
   */
  static std::string findXPathForParagraph(const std::shared_ptr<Epub>& epub, int spineIndex, uint16_t paragraphIndex);

  /**
   * Resolve intra-spine progress to a real XHTML ancestry path plus text offset.
   *
   * Returns a KOReader-compatible path like:
   * /body/DocFragment[8]/body/div[2]/section[1]/p[4]/text().96
   *
   * An empty string means parsing failed or the location could not be resolved.
   */
  static std::string findXPathForProgress(const std::shared_ptr<Epub>& epub, int spineIndex, float intraSpineProgress);

  /**
   * Resolve an absolute visible-codepoint offset within a spine item to its
   * KOReader XPath, including the /text()[k].NN suffix.
   *
   * THE FRAME OF visibleCharOffset, because getting it wrong is worth less than a
   * page and therefore survives review:
   *
   *   - ZERO-BASED. An offset of N means N codepoints precede the character the
   *     position sits on; N is that character's index, not a 1-based ordinal.
   *   - Counted over EVERY character data event inside <body> that is outside a
   *     non-visible element (VisibleTextUtils::isNonVisibleElement), WHITESPACE
   *     INCLUDED. Inter-tag whitespace in a pretty-printed EPUB is real character
   *     data and it counts; a heading, a <td> or loose text in a <div> counts too.
   *   - Codepoints, not bytes and not glyphs.
   *
   * That is exactly what ChapterHtmlSlimParser::characterData stamps into
   * Page::visibleTextOffset, and what ParagraphStreamer counts on the inbound path.
   * Passing an offset produced under any other model silently shifts the result.
   * See the long note in ChapterXPathParsers.h.
   *
   * spineIndex is zero-based; the DocFragment[N] it emits is one-based, as are the
   * p[N] and text()[k] indices, per XPath.
   *
   * This is the accurate entry point and should be preferred over
   * findXPathForProgress: the offset names an exact character, while a page
   * fraction only names a page. It needs a single pass, because the target is
   * already absolute and does not have to be derived from a chapter total.
   *
   * @param intraSpineProgress optional out-param, set to offset/totalVisibleChars
   *        (a 0..1 intra-chapter fraction, NOT a whole-book percentage) when the
   *        chapter parsed, and to a negative value otherwise, so a caller can derive
   *        its percentage from the same anchor as the XPath.
   *
   * An empty string means parsing failed or the offset lies past the end of the
   * chapter's visible text; callers are expected to fall back.
   */
  static std::string findXPathForOffset(const std::shared_ptr<Epub>& epub, int spineIndex, uint32_t visibleCharOffset,
                                        float* intraSpineProgress = nullptr);
};
