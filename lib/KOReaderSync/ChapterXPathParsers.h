#pragma once

// Internal header: the expat-backed parsers behind ChapterXPathResolver.
//
// These classes live here rather than in an anonymous namespace inside
// ChapterXPathResolver.cpp so the host gtest suite can drive them directly.
// ChapterXPathResolver's public API takes a std::shared_ptr<Epub>, which pulls
// in SD/HAL and is not host-testable; the parsers themselves are plain Print
// sinks and are where all of the XPath logic lives.
//
// Do NOT change the streaming design: the device feeds these in 1 KB chunks
// through readItemContentsToStream() and must keep doing so.

#include <Epub/VisibleTextUtils.h>
#include <Logging.h>
#include <Print.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace chapter_xpath {

inline std::string stripPrefix(const XML_Char* name) {
  if (!name) {
    return "";
  }

  const char* local = std::strrchr(name, ':');
  return local ? std::string(local + 1) : std::string(name);
}

struct NameCounter {
  std::string name;
  int count;
};

struct ParentState {
  std::vector<NameCounter> children;

  int nextIndex(const std::string& name) {
    for (auto& child : children) {
      if (child.name == name) {
        child.count++;
        return child.count;
      }
    }

    children.push_back({name, 1});
    return 1;
  }

  int countOf(const std::string& name) const {
    for (const auto& child : children) {
      if (child.name == name) {
        return child.count;
      }
    }
    return 0;
  }
};

struct PathSegment {
  std::string name;
  int index;
};

// crengine -- which is what KOReader resolves these paths against -- omits the
// positional predicate when an element is the only child of its parent carrying
// that tag name. Real paths read off a device:
//
//   /body/DocFragment[2]/body/div[1]/h1/text().0
//   /body/DocFragment[312]/body/div/div[1]/h2/text().0
//
// so `siblingTotals[i]` must be the FINAL number of children of segment i's parent
// that share segment i's name, not the number seen so far when the element opened.
// A total of 0 means "never resolved" (a truncated or malformed document); the index
// is emitted in that case, which is the conservative pre-Fix-3 behaviour.
inline std::string buildParagraphXPath(const int spineIndex, const std::vector<PathSegment>& path,
                                       const std::vector<int>& siblingTotals, const int textNodeIndex,
                                       const size_t charOffset) {
  std::string xpath = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  for (size_t i = 0; i < path.size(); i++) {
    const int total = (i < siblingTotals.size()) ? siblingTotals[i] : 0;
    xpath += "/" + path[i].name;
    if (total != 1) {
      xpath += "[" + std::to_string(path[i].index) + "]";
    }
  }
  // Offset 0 is a real position -- "the first character of text node k" -- and it is the
  // one findXPathForOffset produces most often, because a page usually begins at the top
  // of a paragraph. Suppressing the suffix there would emit the bare element path, which
  // means "somewhere in this element" and is exactly the one-page-early landing this
  // whole path exists to stop. Real KOReader paths carry it too: /body/DocFragment[2]/
  // body/div[1]/h1/text().0. textNodeIndex == 0 still means "no text node resolved".
  if (textNodeIndex > 0) {
    xpath += "/text()[" + std::to_string(textNodeIndex) + "]." + std::to_string(charOffset);
  }
  return xpath;
}

inline size_t countUtf8Codepoints(const XML_Char* data, const int len) {
  if (!data || len <= 0) {
    return 0;
  }

  size_t count = 0;
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(data);
  const unsigned char* end = ptr + len;
  while (ptr < end) {
    utf8NextCodepoint(&ptr);
    count++;
  }

  return count;
}

// XML whitespace, per the spec's S production. Used ONLY to decide whether a character
// data event may advance the text()[k] index -- never to decide whether it counts toward
// the character offset, which counts every codepoint. See onCharacterData below.
inline bool isXmlWhitespaceOnly(const XML_Char* data, const int len) {
  if (!data || len <= 0) {
    return false;
  }

  for (int i = 0; i < len; i++) {
    const char c = data[i];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
      return false;
    }
  }

  return true;
}

// How to read the target handed to XPathProgressResolver.
enum class TargetMode : uint8_t {
  // A 1-based count of characters consumed, as findXPathForProgress derives from
  // ceil(fraction * total). The target-th character is the last one before the position.
  CharactersConsumed,
  // A zero-based codepoint index, as pagination stamps into Page::visibleTextOffset: an
  // offset of N means N codepoints precede the page's first character, so the position
  // sits ON codepoint N, not after it.
  CodepointIndex,
};

class ParagraphTextCounter final : public Print {
 public:
  ParagraphTextCounter() {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &ParagraphTextCounter::startElement, &ParagraphTextCounter::endElement);
    XML_SetCharacterDataHandler(parser, &ParagraphTextCounter::characterData);
  }

  ~ParagraphTextCounter() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s at byte %ld of %u", XML_ErrorString(XML_GetErrorCode(parser)),
              XML_GetCurrentByteIndex(parser), static_cast<unsigned>(bytesIn));
      parseOk = false;
    }
    return parseOk;
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    // Counted before any early return so the total always reflects what the caller
    // actually streamed in, even once parsing has given up. A device run whose total
    // differs from the item's stored size means the bytes never arrived intact, which
    // is a different bug from expat rejecting bytes that are themselves fine.
    bytesIn += size;
    if (!parser || !parseOk) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s at byte %ld of %u", XML_ErrorString(error), XML_GetCurrentByteIndex(parser),
                static_cast<unsigned>(bytesIn));
        parseOk = false;
      }
    }

    return size;
  }

  size_t totalVisibleChars() const { return visibleChars; }

  // Diagnostics for the caller's failure logging; safe to call after finish().
  const char* errorText() const { return parser ? XML_ErrorString(XML_GetErrorCode(parser)) : "no parser"; }
  long errorByteIndex() const { return parser ? XML_GetCurrentByteIndex(parser) : -1; }
  size_t bytesFed() const { return bytesIn; }

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onEndElement(name);
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onCharacterData(data, len);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
      }
      depth++;
      return;
    }

    if (nonVisibleDepth > 0 || VisibleTextUtils::isNonVisibleElement(name)) {
      nonVisibleDepth++;
    }
    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (nonVisibleDepth > 0) {
      nonVisibleDepth--;
    }
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
    }
  }

  void onCharacterData(const XML_Char* data, const int len) {
    // One counting model, three implementations, and they MUST agree:
    // ChapterHtmlSlimParser::characterData (pagination, which stamps
    // Page::visibleTextOffset), ParagraphStreamer in ProgressMapper.cpp (the inbound
    // path), and this file (the outbound path). All three count every codepoint of
    // every character data event with insideBody && nonVisibleDepth == 0, using the
    // shared VisibleTextUtils::isNonVisibleElement predicate.
    //
    // Two things that look like bugs and are not:
    //  - Whitespace between tags counts. EPUBs are pretty-printed, so the newline and
    //    indent between </p> and <p> are real character data and pagination counts
    //    them; one test chapter carries 135 codepoints of them. Skipping them here
    //    would put every offset out by that much, growing through the chapter.
    //  - Text outside <p>/<li> counts. This used to be <p>-only, then <p>-or-<li>,
    //    which made a heading or loose text in a <div> invisible to the outbound
    //    resolver while pagination counted it.
    if (!insideBody || nonVisibleDepth != 0 || len <= 0) {
      return;
    }

    visibleChars += countUtf8Codepoints(data, len);
  }

 private:
  XML_Parser parser = nullptr;
  bool parseOk = true;
  bool insideBody = false;
  int depth = 0;
  int bodyDepth = -1;
  int nonVisibleDepth = 0;
  size_t visibleChars = 0;
  size_t bytesIn = 0;
};

class XPathParagraphResolver final : public Print {
 public:
  explicit XPathParagraphResolver(const int targetParagraph) : targetParagraph(targetParagraph) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &XPathParagraphResolver::startElement, &XPathParagraphResolver::endElement);
  }

  ~XPathParagraphResolver() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s at byte %ld of %u", XML_ErrorString(XML_GetErrorCode(parser)),
              XML_GetCurrentByteIndex(parser), static_cast<unsigned>(bytesIn));
      parseOk = false;
    }
    return parseOk;
  }

  bool hasMatch() const { return matched; }

  const std::string& getXPath() const {
    if (matched && xpath.empty()) {
      xpath = buildParagraphXPath(spineIndex, matchedPath, matchedSiblingTotals, 0, 0);
    }
    return xpath;
  }

  const char* errorText() const { return parser ? XML_ErrorString(XML_GetErrorCode(parser)) : "no parser"; }
  long errorByteIndex() const { return parser ? XML_GetCurrentByteIndex(parser) : -1; }
  size_t bytesFed() const { return bytesIn; }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    // Counted before any early return so the total always reflects what the caller
    // actually streamed in, even once parsing has given up.
    bytesIn += size;
    if (!parser || !parseOk) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s at byte %ld of %u", XML_ErrorString(error), XML_GetCurrentByteIndex(parser),
                static_cast<unsigned>(bytesIn));
        parseOk = false;
      }
    }

    return size;
  }

  int spineIndex = 0;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<XPathParagraphResolver*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<XPathParagraphResolver*>(userData);
    self->onEndElement(name);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
      }
      depth++;
      return;
    }

    const int siblingIndex = parentStates.back().nextIndex(name);
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();

    // Count both <p> and <li> as paragraph-like positions, matching how the section
    // layout tracks them (xpathParagraphIndex and xpathListItemIndex). This ensures
    // KOReader progress in list items maps to the correct XPath.
    if (name == "p") {
      paragraphCount++;
    } else if (name == "li") {
      paragraphCount++;
    }
    if (!matched && paragraphCount == targetParagraph) {
      recordMatch();
    }

    depth++;
  }

  // Snapshot the ancestry, then keep parsing. The XPath cannot be emitted here: the
  // final sibling counts each segment needs are not known until its parent closes.
  void recordMatch() {
    matched = true;
    matchedPath = path;
    matchedSiblingTotals.assign(path.size(), 0);
  }

  // parentStates[i] is the per-name child counter of path[i]'s parent, so it holds
  // path[i]'s final sibling total at the moment it is popped.
  void captureSiblingTotal(const size_t index) {
    if (!matched || index >= matchedPath.size() || index >= parentStates.size()) {
      return;
    }
    matchedSiblingTotals[index] = parentStates[index].countOf(matchedPath[index].name);
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      for (size_t i = parentStates.size(); i-- > 0;) {
        captureSiblingTotal(i);
      }
      parentStates.clear();
      path.clear();
      return;
    }

    if (!path.empty()) {
      path.pop_back();
    }
    if (!parentStates.empty()) {
      captureSiblingTotal(parentStates.size() - 1);
      parentStates.pop_back();
    }
  }

  XML_Parser parser = nullptr;
  const int targetParagraph;
  bool parseOk = true;
  bool insideBody = false;
  bool matched = false;
  int depth = 0;
  int bodyDepth = -1;
  int paragraphCount = 0;
  size_t bytesIn = 0;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
  std::vector<PathSegment> matchedPath;
  std::vector<int> matchedSiblingTotals;
  mutable std::string xpath;
};

class XPathProgressResolver final : public Print {
 public:
  explicit XPathProgressResolver(const size_t targetVisibleChar, const TargetMode mode = TargetMode::CharactersConsumed)
      : targetVisibleChar(targetVisibleChar), mode(mode) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &XPathProgressResolver::startElement, &XPathProgressResolver::endElement);
    XML_SetCharacterDataHandler(parser, &XPathProgressResolver::characterData);
  }

  ~XPathProgressResolver() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s at byte %ld of %u", XML_ErrorString(XML_GetErrorCode(parser)),
              XML_GetCurrentByteIndex(parser), static_cast<unsigned>(bytesIn));
      parseOk = false;
    }
    return parseOk;
  }

  bool hasMatch() const { return matched; }

  // Valid after finish(). Counting continues past the match -- expat walks the tail
  // anyway, because the final sibling totals are not known until each parent closes --
  // so the total costs one extra codepoint scan of the remaining bytes and no extra
  // SD I/O. It lets the caller derive an intra-chapter fraction from the same anchor
  // that produced the XPath instead of from a page fraction.
  size_t totalVisibleChars() const { return visibleChars; }

  const std::string& getXPath() const {
    if (matched && xpath.empty()) {
      xpath = buildParagraphXPath(spineIndex, matchedPath, matchedSiblingTotals, matchedTextNode, matchedCharOffset);
    }
    return xpath;
  }

  const char* errorText() const { return parser ? XML_ErrorString(XML_GetErrorCode(parser)) : "no parser"; }
  long errorByteIndex() const { return parser ? XML_GetCurrentByteIndex(parser) : -1; }
  size_t bytesFed() const { return bytesIn; }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    // Counted before any early return so the total always reflects what the caller
    // actually streamed in, even once parsing has given up.
    bytesIn += size;
    if (!parser || !parseOk) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s at byte %ld of %u", XML_ErrorString(error), XML_GetCurrentByteIndex(parser),
                static_cast<unsigned>(bytesIn));
        parseOk = false;
      }
    }

    return size;
  }

  int spineIndex = 0;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onEndElement(name);
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onCharacterData(data, len);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
      }
      depth++;
      return;
    }

    const int siblingIndex = parentStates.back().nextIndex(name);
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();
    textNodeIndexStack.push_back(0);
    pendingTextNode = true;

    if (nonVisibleDepth > 0 || VisibleTextUtils::isNonVisibleElement(name)) {
      nonVisibleDepth++;
    }

    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (nonVisibleDepth > 0) {
      nonVisibleDepth--;
    }
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      for (size_t i = parentStates.size(); i-- > 0;) {
        captureSiblingTotal(i);
      }
      parentStates.clear();
      path.clear();
      textNodeIndexStack.clear();
      return;
    }

    if (!textNodeIndexStack.empty()) {
      textNodeIndexStack.pop_back();
    }
    // A closing tag ends whatever text node was open, so the next character data starts
    // a new text node of the parent: <p>a<b>x</b>c</p> gives p text()[1]="a" and
    // text()[2]="c". This used to be conditional on being inside a <p>/<li>, which is
    // no longer the only place text is tracked.
    pendingTextNode = true;
    if (!path.empty()) {
      path.pop_back();
    }
    if (!parentStates.empty()) {
      captureSiblingTotal(parentStates.size() - 1);
      parentStates.pop_back();
    }
  }

  // parentStates[i] is the per-name child counter of path[i]'s parent, so it holds
  // path[i]'s final sibling total at the moment it is popped.
  void captureSiblingTotal(const size_t index) {
    if (!matched || index >= matchedPath.size() || index >= parentStates.size()) {
      return;
    }
    matchedSiblingTotals[index] = parentStates[index].countOf(matchedPath[index].name);
  }

  void onCharacterData(const XML_Char* data, const int len) {
    // Identical predicate to ParagraphTextCounter::onCharacterData above -- see the
    // long note there for why whitespace and non-paragraph text both count.
    if (!insideBody || nonVisibleDepth != 0 || len <= 0) {
      return;
    }

    const size_t codepointCount = countUtf8Codepoints(data, len);
    if (codepointCount == 0) {
      return;
    }

    // TWO COUNTERS, TWO RULES, and they must not be conflated.
    //
    // visibleChars is the character offset. It counts every codepoint, whitespace
    // included, because that is what pagination counts.
    //
    // textNodeIndexStack is the k in text()[k]. crengine drops whitespace-only nodes
    // when it builds its DOM, and KOReader skips the empty nodes a bare <a id="..."/>
    // creates, so only content with real characters advances it. Letting whitespace
    // bump it would send the receiving device to the wrong node entirely, which is a
    // far worse failure than an offset that is a few characters out.
    const bool realText = !isXmlWhitespaceOnly(data, len);
    if (realText && pendingTextNode) {
      if (!textNodeIndexStack.empty()) {
        textNodeIndexStack.back()++;
      }
      textNodeStartChars = visibleChars;
      pendingTextNode = false;
    }

    const size_t nextVisibleChars = visibleChars + codepointCount;
    // CodepointIndex is zero-based and the position sits ON the target codepoint, so
    // the comparison is strict: an offset equal to the running total starts the NEXT
    // node rather than pointing one past the end of this one. CharactersConsumed is
    // 1-based and inclusive, which is what findXPathForProgress has always meant.
    const bool reached = (mode == TargetMode::CodepointIndex) ? (targetVisibleChar < nextVisibleChars)
                                                              : (targetVisibleChar <= nextVisibleChars);

    // Only real text can be matched. A target that lands inside a whitespace-only run
    // between two blocks has no character at it, and a path into a node crengine never
    // built resolves to nothing; std::max advances such a target to offset 0 of the
    // first real text node at or after it.
    if (!matched && realText && reached) {
      // Snapshot the ancestry and keep parsing. The XPath cannot be emitted here: the
      // final sibling counts each segment needs are not known until its parent closes,
      // so the parser is deliberately NOT stopped. Streaming already reads the whole
      // spine item anyway (readItemContentsToStream's allowEarlyStop is false), so the
      // only extra cost is expat walking the tail -- no extra SD I/O.
      const size_t hitChar = std::max(targetVisibleChar, visibleChars);
      matched = true;
      matchedPath = path;
      matchedSiblingTotals.assign(path.size(), 0);
      matchedTextNode = textNodeIndexStack.empty() ? 0 : textNodeIndexStack.back();
      matchedCharOffset = hitChar - textNodeStartChars;
    }

    visibleChars = nextVisibleChars;
  }

  XML_Parser parser = nullptr;
  const size_t targetVisibleChar;
  const TargetMode mode;
  bool parseOk = true;
  bool insideBody = false;
  bool matched = false;
  bool pendingTextNode = true;
  int depth = 0;
  int bodyDepth = -1;
  int nonVisibleDepth = 0;
  int matchedTextNode = 0;
  size_t visibleChars = 0;
  size_t textNodeStartChars = 0;
  size_t matchedCharOffset = 0;
  size_t bytesIn = 0;
  std::vector<int> textNodeIndexStack;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
  std::vector<PathSegment> matchedPath;
  std::vector<int> matchedSiblingTotals;
  mutable std::string xpath;
};

}  // namespace chapter_xpath
