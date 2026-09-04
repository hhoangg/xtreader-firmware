#include "ChapterXPathResolver.h"

#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "ChapterXPathParsers.h"

using chapter_xpath::ParagraphTextCounter;
using chapter_xpath::TargetMode;
using chapter_xpath::XPathParagraphResolver;
using chapter_xpath::XPathProgressResolver;

namespace {
// Both entry points below have several distinct ways to return "", and on hardware
// they used to share one log line, so a device that fell through to
// ProgressMapper::generateXPath could not say which branch it died on. Every branch
// now names its own condition. The byte totals matter as much as the error text: the
// parsers count what they were actually fed, so a total that disagrees with the item's
// stored size means the bytes never arrived intact (a ZipFile inflation problem),
// while a matching total plus a parse error means expat rejected bytes that are
// themselves fine. These are LOG_DBG/LOG_ERR and compile out of a release build with
// every other one; do not promote them.
void logSpineItem(const std::shared_ptr<Epub>& epub, const char* what, const int spineIndex, const std::string& href) {
// getItemSize() re-reads the zip central directory, so unlike the LOG_DBG arguments
// elsewhere in this file it does not vanish with the macro. Guard it explicitly so a
// release build pays nothing for diagnostics it cannot print.
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  size_t itemSize = 0;
  const bool sized = epub->getItemSize(href, &itemSize);
  LOG_DBG("KOX", "%s spine %d href=%s size=%s%u", what, spineIndex, href.c_str(),
          sized ? "" : "unknown:", static_cast<unsigned>(itemSize));
#else
  (void)epub;
  (void)what;
  (void)spineIndex;
  (void)href;
#endif
}
}  // namespace

std::string ChapterXPathResolver::findXPathForParagraph(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                        const uint16_t paragraphIndex) {
  if (!epub || paragraphIndex == 0 || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    LOG_DBG("KOX", "Paragraph lookup: spine %d has an empty href", spineIndex);
    return "";
  }

  logSpineItem(epub, "Paragraph lookup:", spineIndex, href);

  XPathParagraphResolver resolver(paragraphIndex);
  if (!resolver.ok()) {
    LOG_DBG("KOX", "Paragraph lookup: XML parser allocation failed for spine %d", spineIndex);
    return "";
  }

  resolver.spineIndex = spineIndex;
  if (!epub->readItemContentsToStream(href, resolver, 1024)) {
    LOG_DBG("KOX", "Paragraph lookup: %s did not stream (%u bytes reached the parser)", href.c_str(),
            static_cast<unsigned>(resolver.bytesFed()));
    return "";
  }
  if (!resolver.finish()) {
    LOG_DBG("KOX", "Paragraph lookup: XML parse error in %s: %s at byte %ld of %u fed", href.c_str(),
            resolver.errorText(), resolver.errorByteIndex(), static_cast<unsigned>(resolver.bytesFed()));
    return "";
  }

  if (resolver.hasMatch()) {
    LOG_DBG("KOX", "Resolved paragraph %u in spine %d after %u bytes -> %s", paragraphIndex, spineIndex,
            static_cast<unsigned>(resolver.bytesFed()), resolver.getXPath().c_str());
    return resolver.getXPath();
  }

  LOG_DBG("KOX", "Paragraph %u not found in spine %d (%u bytes parsed cleanly)", paragraphIndex, spineIndex,
          static_cast<unsigned>(resolver.bytesFed()));
  return "";
}

std::string ChapterXPathResolver::findXPathForProgress(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                       const float intraSpineProgress) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    LOG_DBG("KOX", "Progress lookup: spine %d has an empty href", spineIndex);
    return "";
  }

  if (!(intraSpineProgress > 0.0f)) {
    return "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  }

  logSpineItem(epub, "Progress lookup:", spineIndex, href);

  ParagraphTextCounter counter;
  if (!counter.ok()) {
    LOG_DBG("KOX", "Progress lookup: XML parser allocation failed for spine %d (pass 1)", spineIndex);
    return "";
  }
  if (!epub->readItemContentsToStream(href, counter, 1024)) {
    LOG_DBG("KOX", "Progress lookup: %s did not stream on pass 1 (%u bytes reached the parser)", href.c_str(),
            static_cast<unsigned>(counter.bytesFed()));
    return "";
  }
  if (!counter.finish()) {
    LOG_DBG("KOX", "Progress lookup: pass 1 XML parse error in %s: %s at byte %ld of %u fed", href.c_str(),
            counter.errorText(), counter.errorByteIndex(), static_cast<unsigned>(counter.bytesFed()));
    return "";
  }

  const size_t totalVisibleChars = counter.totalVisibleChars();
  if (totalVisibleChars == 0) {
    LOG_DBG("KOX", "Progress lookup: pass 1 parsed %u bytes of %s but counted 0 visible chars",
            static_cast<unsigned>(counter.bytesFed()), href.c_str());
    return "";
  }

  const float clamped = std::max(0.0f, std::min(1.0f, intraSpineProgress));
  const size_t targetVisibleChar =
      std::max<size_t>(1, std::min(totalVisibleChars, static_cast<size_t>(std::ceil(clamped * totalVisibleChars))));

  XPathProgressResolver resolver(targetVisibleChar);
  if (!resolver.ok()) {
    LOG_DBG("KOX", "Progress lookup: XML parser allocation failed for spine %d (pass 2)", spineIndex);
    return "";
  }

  resolver.spineIndex = spineIndex;
  if (!epub->readItemContentsToStream(href, resolver, 1024)) {
    LOG_DBG("KOX", "Progress lookup: %s did not stream on pass 2 (%u bytes reached the parser)", href.c_str(),
            static_cast<unsigned>(resolver.bytesFed()));
    return "";
  }
  if (!resolver.finish()) {
    LOG_DBG("KOX", "Progress lookup: pass 2 XML parse error in %s: %s at byte %ld of %u fed", href.c_str(),
            resolver.errorText(), resolver.errorByteIndex(), static_cast<unsigned>(resolver.bytesFed()));
    return "";
  }

  if (resolver.hasMatch()) {
    LOG_DBG("KOX", "Resolved progress %.3f in spine %d (char %u/%u, %u bytes) -> %s", intraSpineProgress, spineIndex,
            static_cast<unsigned>(targetVisibleChar), static_cast<unsigned>(totalVisibleChars),
            static_cast<unsigned>(resolver.bytesFed()), resolver.getXPath().c_str());
    return resolver.getXPath();
  }

  LOG_DBG("KOX", "Progress lookup: pass 2 reached the end of %s without hitting char %u of %u (%u bytes parsed)",
          href.c_str(), static_cast<unsigned>(targetVisibleChar), static_cast<unsigned>(totalVisibleChars),
          static_cast<unsigned>(resolver.bytesFed()));
  return "";
}

std::string ChapterXPathResolver::findXPathForOffset(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                     const uint32_t visibleCharOffset, float* intraSpineProgress) {
  if (intraSpineProgress) {
    // Negative means "no opinion" -- the caller keeps whatever intra it already had.
    *intraSpineProgress = -1.0f;
  }

  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    LOG_DBG("KOX", "Offset lookup: spine %d (0-based) has an empty href", spineIndex);
    return "";
  }

  logSpineItem(epub, "Offset lookup:", spineIndex, href);

  // One pass, unlike findXPathForProgress. That entry point needs a first pass only to
  // turn a fraction into an absolute character; here the caller already handed us the
  // absolute character, so the counting pass has nothing to contribute.
  XPathProgressResolver resolver(visibleCharOffset, TargetMode::CodepointIndex);
  if (!resolver.ok()) {
    LOG_DBG("KOX", "Offset lookup: XML parser allocation failed for spine %d (0-based)", spineIndex);
    return "";
  }

  resolver.spineIndex = spineIndex;
  if (!epub->readItemContentsToStream(href, resolver, 1024)) {
    LOG_DBG("KOX", "Offset lookup: %s did not stream (%u bytes reached the parser)", href.c_str(),
            static_cast<unsigned>(resolver.bytesFed()));
    return "";
  }
  if (!resolver.finish()) {
    LOG_DBG("KOX", "Offset lookup: XML parse error in %s: %s at byte %ld of %u fed", href.c_str(), resolver.errorText(),
            resolver.errorByteIndex(), static_cast<unsigned>(resolver.bytesFed()));
    return "";
  }

  const size_t totalVisibleChars = resolver.totalVisibleChars();
  if (intraSpineProgress && totalVisibleChars > 0) {
    *intraSpineProgress = std::min(1.0f, static_cast<float>(visibleCharOffset) / static_cast<float>(totalVisibleChars));
  }

  if (resolver.hasMatch()) {
    LOG_DBG("KOX",
            "Resolved offset=%u (0-based, body-visible incl. whitespace) of %u such chars in spine %d (0-based), "
            "%u bytes fed -> %s",
            static_cast<unsigned>(visibleCharOffset), static_cast<unsigned>(totalVisibleChars), spineIndex,
            static_cast<unsigned>(resolver.bytesFed()), resolver.getXPath().c_str());
    return resolver.getXPath();
  }

  // Past the end of the chapter's visible text, or the whole tail after the offset was
  // whitespace. Deliberately "" rather than a synthesised last position: the caller has
  // real fallbacks (paragraph index, then page fraction) and they are better evidence
  // than a guess made here.
  LOG_DBG("KOX",
          "Offset lookup: %s holds %u body-visible chars (incl. whitespace) and never reached offset=%u "
          "(0-based, same frame); %u bytes parsed",
          href.c_str(), static_cast<unsigned>(totalVisibleChars), static_cast<unsigned>(visibleCharOffset),
          static_cast<unsigned>(resolver.bytesFed()));
  return "";
}
