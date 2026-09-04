#include <gtest/gtest.h>

#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

#include "ChapterXPathParsers.h"

using chapter_xpath::ParagraphTextCounter;
using chapter_xpath::TargetMode;
using chapter_xpath::XPathParagraphResolver;
using chapter_xpath::XPathProgressResolver;

namespace {

// The device never hands these parsers a whole document: Epub::readItemContentsToStream
// pushes the spine item through Print::write() in 1 KB slices. Every test feeds in
// chunks for the same reason, and several sweep the chunk size so that a chunk boundary
// lands inside a multi-byte UTF-8 sequence and inside an entity reference.
void feed(Print& sink, const std::string& xml, const size_t chunkSize) {
  for (size_t offset = 0; offset < xml.size(); offset += chunkSize) {
    const size_t take = std::min(chunkSize, xml.size() - offset);
    sink.write(reinterpret_cast<const uint8_t*>(xml.data()) + offset, take);
  }
}

constexpr int kRealSpineIndex = 441;  // OEBPS/page-438.html -> DocFragment[442]
constexpr int kParagraphCount = 74;
constexpr size_t kCharsPerParagraph = 10;

// The shape of the spine item that produced the bad "/body/DocFragment[442]/body/p[8]"
// on hardware: one <div> under <body>, and all 74 <p> at depth 4.
std::string realChapterShape() {
  std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>)";
  xml += R"(<html xmlns="http://www.w3.org/1999/xhtml"><head><title>t</title></head>)";
  xml += R"(<body><div class="chapter"><div class="wrap"><h2>Chapter Title</h2></div>)";
  xml += R"(<div class="ugc"><div>)";
  for (int i = 0; i < kParagraphCount; i++) {
    xml += "<p>ABCDEFGHIJ</p>";
  }
  xml += "</div></div></div></body></html>";
  return xml;
}

constexpr size_t kRealChapterHeadingChars = 13;  // "Chapter Title" in realChapterShape()

// ---------------------------------------------------------------------------------
// The counting-model fixture.
//
// The outbound resolver used to count only text inside <p>/<li>, while pagination --
// which stamps the Page::visibleTextOffset the resolver is now fed -- counts every
// codepoint with insideBody && nonVisibleDepth == 0. This fixture is built so the two
// models disagree by a known, hand-checkable amount:
//
//   <h2> heading                                     28 codepoints
//   loose text directly in the <div>, before any <p> 41 codepoints
//   pretty-printing whitespace between blocks        76 gaps x 3 = 228 codepoints
//   <style> inside <body>                             0 (non-visible, must be skipped)
//   paragraph text, across 74 paragraphs      10,821 codepoints
//
// The paragraph lengths are the real book's: with the <p>-only model p[10] spans
// 1926..2313 (len 388), p[11] 2314..2804 (len 491) and p[12] 2805..2985 (len 181),
// 1-based, out of 10,821 across 74 paragraphs.
constexpr size_t kAlignedHeadingChars = 28;
constexpr size_t kAlignedLooseDivChars = 41;
constexpr size_t kAlignedGapChars = 3;   // "\n  "
constexpr size_t kAlignedGapCount = 76;  // one after the <h2>, one before each <p>, one trailing
constexpr size_t kAlignedParagraphChars = 10821;
constexpr size_t kAlignedWhitespaceChars = kAlignedGapChars * kAlignedGapCount;
constexpr size_t kAlignedTotalChars =
    kAlignedHeadingChars + kAlignedLooseDivChars + kAlignedWhitespaceChars + kAlignedParagraphChars;

// Everything that precedes p[1]'s first character in the aligned model.
constexpr size_t kAlignedFirstParagraphStart = kAlignedHeadingChars + kAlignedGapChars + kAlignedLooseDivChars;

// Printable filler with no markup-significant characters, so the codepoint count is
// exactly n and no entity decoding is involved.
std::string filler(const size_t n) {
  std::string out;
  out.reserve(n);
  for (size_t i = 0; i < n; i++) {
    out += static_cast<char>('a' + (i % 26));
  }
  return out;
}

const std::vector<size_t>& alignedParagraphLengths() {
  static const std::vector<size_t> lengths = [] {
    std::vector<size_t> v;
    v.insert(v.end(), 8, 214);   // p[1]..p[8]
    v.push_back(213);            // p[9]   -- 1925 codepoints precede p[10]
    v.push_back(388);            // p[10]  -- the measured 1926..2313
    v.push_back(491);            // p[11]  -- the measured 2314..2804
    v.push_back(181);            // p[12]  -- the measured 2805..2985
    v.insert(v.end(), 61, 128);  // p[13]..p[73]
    v.push_back(28);             // p[74]  -- brings the paragraph total to 10,821
    return v;
  }();
  return lengths;
}

// Zero-based offset of the first character of the 1-based paragraph k, in the ALIGNED
// model: heading + gap + loose text, then one gap and one paragraph per predecessor.
size_t alignedParagraphStart(const int k) {
  const auto& lengths = alignedParagraphLengths();
  size_t offset = kAlignedFirstParagraphStart;
  for (int i = 0; i < k - 1; i++) {
    offset += kAlignedGapChars + lengths[static_cast<size_t>(i)];
  }
  return offset + kAlignedGapChars;
}

std::string alignedChapterShape() {
  const std::string gap = "\n  ";
  std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>)";
  xml += R"(<html xmlns="http://www.w3.org/1999/xhtml"><head><title>ignored</title></head>)";
  xml += R"(<body><div class="chapter">)";
  // Inside <body>, so only the shared isNonVisibleElement predicate keeps it out.
  xml += R"(<style>p { margin: 0 }</style>)";
  xml += "<h2>" + filler(kAlignedHeadingChars) + "</h2>";
  xml += gap;
  xml += filler(kAlignedLooseDivChars);
  for (const size_t len : alignedParagraphLengths()) {
    xml += gap;
    xml += "<p>" + filler(len) + "</p>";
  }
  xml += gap;
  xml += "</div></body></html>";
  return xml;
}

std::string readFile(const char* path) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return "";
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

}  // namespace

// --- ParagraphTextCounter (pass 1) -------------------------------------------------

// If this ever fails, the device bug has been reproduced off-device.
TEST(ParagraphTextCounter, RealChapterShapeCountsItsParagraphs) {
  ParagraphTextCounter counter;
  ASSERT_TRUE(counter.ok());
  feed(counter, realChapterShape(), 1024);
  ASSERT_TRUE(counter.finish());
  EXPECT_GT(counter.totalVisibleChars(), 0u);
  // The <h2> counts now. It did not under the old <p>-only rule, and pagination has
  // always counted it, which is precisely the divergence this alignment removes.
  EXPECT_EQ(counter.totalVisibleChars(), kParagraphCount * kCharsPerParagraph + kRealChapterHeadingChars);
}

// The counting model, stated as a rule rather than a total: text is counted when it is
// inside <body> and outside every non-visible element, whatever tag encloses it. <title>
// is excluded because isNonVisibleElement says so, not because it sits before <body>.
TEST(ParagraphTextCounter, CountsEveryVisibleTagAndSkipsOnlyTheNonVisibleOnes) {
  ParagraphTextCounter counter;
  ASSERT_TRUE(counter.ok());
  feed(counter,
       "<html><head><title>skipped</title></head><body><h1>kept123</h1><style>skipped</style>"
       "<div>loose</div><p>kept</p></body></html>",
       8);
  ASSERT_TRUE(counter.finish());
  // "kept123" (7) + "loose" (5) + "kept" (4). The <p>-only rule gave 4.
  EXPECT_EQ(counter.totalVisibleChars(), 16u);
}

// Fix 4: pass 1 must count the same stream as pass 2, which has always counted <li>.
TEST(ParagraphTextCounter, CountsListItemTextTheSameWayPassTwoDoes) {
  ParagraphTextCounter counter;
  ASSERT_TRUE(counter.ok());
  feed(counter, "<html><body><ul><li>aaaaa</li><li>bbbbb</li></ul></body></html>", 7);
  ASSERT_TRUE(counter.finish());
  EXPECT_EQ(counter.totalVisibleChars(), 10u);
}

TEST(ParagraphTextCounter, ChunkBoundaryInsideMultiByteUtf8AndEntityDoesNotChangeTheCount) {
  // "é" is 2 bytes, "€" is 3, "𝄞" is 4, and "&gt;" is a 4-byte entity reference that
  // decodes to a single character. Sweeping every chunk size guarantees a boundary
  // inside each of them.
  const std::string xml = "<html><body><p>aé b€ c\xF0\x9D\x84\x9E d&gt;e</p></body></html>";
  size_t expected = 0;
  {
    ParagraphTextCounter counter;
    ASSERT_TRUE(counter.ok());
    feed(counter, xml, xml.size());
    ASSERT_TRUE(counter.finish());
    expected = counter.totalVisibleChars();
  }
  ASSERT_GT(expected, 0u);

  for (size_t chunk = 1; chunk <= xml.size(); chunk++) {
    ParagraphTextCounter counter;
    ASSERT_TRUE(counter.ok());
    feed(counter, xml, chunk);
    ASSERT_TRUE(counter.finish()) << "chunk size " << chunk;
    EXPECT_EQ(counter.totalVisibleChars(), expected) << "chunk size " << chunk;
  }
}

// --- XPathProgressResolver (pass 2) ------------------------------------------------

// The real failure. The emitted path must be resolvable against crengine's DOM.
TEST(XPathProgressResolver, ResolvesTheRealChapterShapeToTheNestedParagraph) {
  // The <h2> (13 chars) plus 7 whole paragraphs (70 chars) precede p[8], so the target
  // that sits 5 chars into p[8] is 88. It was 75 while the counter ignored headings.
  XPathProgressResolver resolver(kRealChapterHeadingChars + 75);
  ASSERT_TRUE(resolver.ok());
  resolver.spineIndex = kRealSpineIndex;
  feed(resolver, realChapterShape(), 1024);
  ASSERT_TRUE(resolver.finish());
  ASSERT_TRUE(resolver.hasMatch());
  EXPECT_EQ(resolver.getXPath(), "/body/DocFragment[442]/body/div/div[2]/div/p[8]/text()[1].5");
  EXPECT_EQ(resolver.getXPath().find("/body/p["), std::string::npos);
}

// Fix 3: crengine omits the index when an element is its parent's only child of that
// tag name. One document, both cases: <section> is unique, <p> is repeated.
TEST(XPathProgressResolver, OmitsTheIndexOnAUniquelyNamedSiblingButKeepsItOnARepeatedOne) {
  XPathProgressResolver resolver(9);  // "alpha" is 5 chars, so char 9 is 4 into "beta"
  ASSERT_TRUE(resolver.ok());
  feed(resolver, "<html><body><section><p>alpha</p><p>beta</p></section></body></html>", 6);
  ASSERT_TRUE(resolver.finish());
  ASSERT_TRUE(resolver.hasMatch());
  EXPECT_EQ(resolver.getXPath(), "/body/DocFragment[1]/body/section/p[2]/text()[1].4");
}

TEST(XPathProgressResolver, KeepsTheIndexWhenTheUniqueLookingParentGainsASiblingLater) {
  // <div> looks unique while p[1] is being parsed, but a second <div> follows. The
  // index must reflect the final sibling count, not the count seen so far.
  XPathProgressResolver resolver(3);
  ASSERT_TRUE(resolver.ok());
  feed(resolver, "<html><body><div><p>aaaaa</p></div><div><p>bbbbb</p></div></body></html>", 5);
  ASSERT_TRUE(resolver.finish());
  ASSERT_TRUE(resolver.hasMatch());
  EXPECT_EQ(resolver.getXPath(), "/body/DocFragment[1]/body/div[1]/p/text()[1].3");
}

// Fix 4: the target derived from pass 1 and the position found by pass 2 must agree
// in a chapter whose text lives in <li>.
TEST(XPathProgressResolver, ListChapterTargetFromPassOneLandsWherePassTwoExpects) {
  const std::string xml = "<html><body><ul><li>aaaaa</li><li>bbbbb</li></ul></body></html>";

  ParagraphTextCounter counter;
  ASSERT_TRUE(counter.ok());
  feed(counter, xml, 1024);
  ASSERT_TRUE(counter.finish());
  const size_t total = counter.totalVisibleChars();
  ASSERT_EQ(total, 10u);

  // Halfway through the chapter is the end of the first list item, not past it.
  const size_t target = static_cast<size_t>(std::ceil(0.5f * static_cast<float>(total)));
  ASSERT_EQ(target, 5u);

  XPathProgressResolver resolver(target);
  ASSERT_TRUE(resolver.ok());
  feed(resolver, xml, 1024);
  ASSERT_TRUE(resolver.finish());
  ASSERT_TRUE(resolver.hasMatch());
  EXPECT_EQ(resolver.getXPath(), "/body/DocFragment[1]/body/ul/li[1]/text()[1].5");
}

TEST(XPathProgressResolver, ChunkBoundarySweepDoesNotChangeTheResolvedPath) {
  const std::string xml = "<html><body><div><p>aé b€ c&gt;d</p><p>tail</p></div></body></html>";
  std::string expected;
  {
    XPathProgressResolver resolver(9);
    ASSERT_TRUE(resolver.ok());
    feed(resolver, xml, xml.size());
    ASSERT_TRUE(resolver.finish());
    ASSERT_TRUE(resolver.hasMatch());
    expected = resolver.getXPath();
  }

  for (size_t chunk = 1; chunk <= xml.size(); chunk++) {
    XPathProgressResolver resolver(9);
    ASSERT_TRUE(resolver.ok());
    feed(resolver, xml, chunk);
    ASSERT_TRUE(resolver.finish()) << "chunk size " << chunk;
    ASSERT_TRUE(resolver.hasMatch()) << "chunk size " << chunk;
    EXPECT_EQ(resolver.getXPath(), expected) << "chunk size " << chunk;
  }
}

// --- TargetMode::CodepointIndex: the outbound offset path ---------------------------
//
// Every offset in these tests is ZERO-BASED and counted over all character data inside
// <body> that is outside a non-visible element, whitespace included -- the frame
// ChapterHtmlSlimParser stamps into Page::visibleTextOffset. An offset of N means N
// codepoints precede the character the position sits on. Paragraph indices p[N] and
// text node indices text()[k] are one-based, as XPath requires.

// THE LOAD-BEARING TEST. Under the old <p>-only rule the resolver counted 10,821 here
// while pagination counted 11,118, so an offset handed over from pagination pointed 297
// codepoints too far into the chapter -- and the gap grows with every heading, loose
// <div> line and pretty-printed newline before the reader's position.
TEST(ParagraphTextCounter, CountsTheAlignedChapterExactlyAsPaginationWould) {
  ParagraphTextCounter counter;
  ASSERT_TRUE(counter.ok());
  feed(counter, alignedChapterShape(), 1024);
  ASSERT_TRUE(counter.finish());

  EXPECT_EQ(counter.totalVisibleChars(), kAlignedTotalChars);
  EXPECT_NE(counter.totalVisibleChars(), kAlignedParagraphChars) << "this is the old <p>-only total";
  EXPECT_EQ(counter.totalVisibleChars() - kAlignedParagraphChars,
            kAlignedHeadingChars + kAlignedLooseDivChars + kAlignedWhitespaceChars);
}

// Whitespace-only text nodes are real character data and pagination counts them, so the
// aligned counter must too. Stated on its own because it is the half of the alignment
// that is easiest to "tidy" away with an isAllWhitespace skip.
TEST(ParagraphTextCounter, CountsWhitespaceBetweenBlocks) {
  ParagraphTextCounter counter;
  ASSERT_TRUE(counter.ok());
  feed(counter, "<html><body><div>\n  <p>alpha</p>\n  <p>beta</p>\n</div></body></html>", 5);
  ASSERT_TRUE(counter.finish());
  // "alpha" (5) + "beta" (4) + two "\n  " gaps (6) + the "\n" before </div> (1).
  EXPECT_EQ(counter.totalVisibleChars(), 16u);
}

// The zero-based/one-based pin, in both directions, on markup with no whitespace so the
// two readings genuinely differ. Under a one-based reading offset 5 would resolve to
// p[1] at offset 5 -- one past the end of "alpha" -- instead of the start of p[2].
TEST(XPathProgressResolver, CodepointIndexIsZeroBasedAtBothEndsOfAParagraph) {
  const std::string xml = "<html><body><p>alpha</p><p>beta</p></body></html>";

  struct Case {
    size_t offset;
    const char* expected;
  };
  const Case cases[] = {
      {0, "/body/DocFragment[1]/body/p[1]/text()[1].0"},
      {4, "/body/DocFragment[1]/body/p[1]/text()[1].4"},  // last character of "alpha"
      {5, "/body/DocFragment[1]/body/p[2]/text()[1].0"},  // first character of "beta"
      {8, "/body/DocFragment[1]/body/p[2]/text()[1].3"},  // last character of "beta"
  };

  for (const auto& c : cases) {
    XPathProgressResolver resolver(c.offset, TargetMode::CodepointIndex);
    ASSERT_TRUE(resolver.ok());
    feed(resolver, xml, 7);
    ASSERT_TRUE(resolver.finish()) << "offset " << c.offset;
    ASSERT_TRUE(resolver.hasMatch()) << "offset " << c.offset;
    EXPECT_EQ(resolver.getXPath(), c.expected) << "offset " << c.offset;
  }
}

// An offset landing mid-paragraph, on the real book's measured p[11].
TEST(XPathProgressResolver, MidParagraphOffsetCarriesTheCharacterSuffix) {
  const size_t start = alignedParagraphStart(11);
  XPathProgressResolver resolver(start + 100, TargetMode::CodepointIndex);
  ASSERT_TRUE(resolver.ok());
  resolver.spineIndex = kRealSpineIndex;
  feed(resolver, alignedChapterShape(), 1024);
  ASSERT_TRUE(resolver.finish());
  ASSERT_TRUE(resolver.hasMatch());
  EXPECT_EQ(resolver.getXPath(), "/body/DocFragment[442]/body/div/p[11]/text()[1].100");
  EXPECT_EQ(resolver.totalVisibleChars(), kAlignedTotalChars);
}

// An offset on a paragraph's first character. The suffix must still be emitted: ".0"
// means "the first character of text node 1", while the bare element path means only
// "somewhere in p[11]", which is what put a receiving device at the top of the paragraph
// instead of where the reader was.
TEST(XPathProgressResolver, OffsetOnAParagraphsFirstCharacterKeepsTheZeroSuffix) {
  XPathProgressResolver resolver(alignedParagraphStart(11), TargetMode::CodepointIndex);
  ASSERT_TRUE(resolver.ok());
  resolver.spineIndex = kRealSpineIndex;
  feed(resolver, alignedChapterShape(), 1024);
  ASSERT_TRUE(resolver.finish());
  ASSERT_TRUE(resolver.hasMatch());
  EXPECT_EQ(resolver.getXPath(), "/body/DocFragment[442]/body/div/p[11]/text()[1].0");
}

// Text whose nearest ancestor is not a <p> or <li> is a legitimate destination now.
TEST(XPathProgressResolver, OffsetInsideAHeadingResolvesToTheHeadingNotAParagraph) {
  XPathProgressResolver resolver(10, TargetMode::CodepointIndex);
  ASSERT_TRUE(resolver.ok());
  resolver.spineIndex = kRealSpineIndex;
  feed(resolver, alignedChapterShape(), 1024);
  ASSERT_TRUE(resolver.finish());
  ASSERT_TRUE(resolver.hasMatch());
  EXPECT_EQ(resolver.getXPath(), "/body/DocFragment[442]/body/div/h2/text()[1].10");
}

// Whitespace counts toward the offset but is not a place a position can be: crengine
// never builds a node for it. The target advances to the first real text at or after it.
TEST(XPathProgressResolver, OffsetInsideAWhitespaceGapAdvancesToTheNextRealText) {
  // "alpha" is 0..4, the "\n  " gap is 5..7, "beta" is 8..11.
  XPathProgressResolver resolver(6, TargetMode::CodepointIndex);
  ASSERT_TRUE(resolver.ok());
  feed(resolver, "<html><body><div><p>alpha</p>\n  <p>beta</p></div></body></html>", 9);
  ASSERT_TRUE(resolver.finish());
  ASSERT_TRUE(resolver.hasMatch());
  EXPECT_EQ(resolver.getXPath(), "/body/DocFragment[1]/body/div/p[2]/text()[1].0");
  EXPECT_EQ(resolver.getXPath().find("/div/text()"), std::string::npos);
}

// ... and a whitespace-only node must not consume a text()[k] index, or the receiving
// device resolves to the wrong node entirely. p keeps text()[1] and text()[2] across the
// intervening <em>, regardless of the whitespace around it.
TEST(XPathProgressResolver, WhitespaceDoesNotAdvanceTheTextNodeIndex) {
  const std::string xml = "<html><body><p> <em>x</em> tail</p></body></html>";
  // " " 0, "x" 1, " tail" 2..6. The target is the "t" of "tail" at offset 3.
  XPathProgressResolver resolver(3, TargetMode::CodepointIndex);
  ASSERT_TRUE(resolver.ok());
  feed(resolver, xml, 6);
  ASSERT_TRUE(resolver.finish());
  ASSERT_TRUE(resolver.hasMatch());
  // The leading " " was skipped, so " tail" is p's text()[1], not text()[2]. It begins
  // at offset 2, so the "t" of "tail" is 1 codepoint in.
  EXPECT_EQ(resolver.getXPath(), "/body/DocFragment[1]/body/p/text()[1].1");
}

// Past the end of the chapter's visible text. The chosen degradation is "no match", so
// ChapterXPathResolver::findXPathForOffset returns "" and ProgressMapper falls through
// to the paragraph index and then the page fraction -- real evidence rather than a
// synthesised last position.
TEST(XPathProgressResolver, OffsetPastTheEndOfTheChapterDoesNotMatchAndDoesNotCrash) {
  for (const size_t offset : {kAlignedTotalChars, kAlignedTotalChars + 1, kAlignedTotalChars * 4}) {
    XPathProgressResolver resolver(offset, TargetMode::CodepointIndex);
    ASSERT_TRUE(resolver.ok());
    resolver.spineIndex = kRealSpineIndex;
    feed(resolver, alignedChapterShape(), 1024);
    ASSERT_TRUE(resolver.finish()) << "offset " << offset;
    EXPECT_FALSE(resolver.hasMatch()) << "offset " << offset;
    EXPECT_TRUE(resolver.getXPath().empty()) << "offset " << offset;
    EXPECT_EQ(resolver.totalVisibleChars(), kAlignedTotalChars) << "offset " << offset;
  }
}

// Change 4: the intra-chapter fraction the caller derives for its percentage comes from
// the same anchor as the XPath, so the two cannot disagree.
TEST(XPathProgressResolver, ReportsTheChapterTotalSoThePercentageSharesTheAnchor) {
  const size_t start = alignedParagraphStart(12);
  XPathProgressResolver resolver(start, TargetMode::CodepointIndex);
  ASSERT_TRUE(resolver.ok());
  feed(resolver, alignedChapterShape(), 1024);
  ASSERT_TRUE(resolver.finish());
  ASSERT_TRUE(resolver.hasMatch());
  ASSERT_EQ(resolver.totalVisibleChars(), kAlignedTotalChars);

  const float intra = static_cast<float>(start) / static_cast<float>(resolver.totalVisibleChars());
  EXPECT_GT(intra, 0.0f);
  EXPECT_LT(intra, 1.0f);
}

TEST(XPathProgressResolver, CodepointIndexChunkBoundarySweepDoesNotChangeTheResolvedPath) {
  const std::string xml =
      "<html><body><div>\n  <p>a\xC3\xA9 b\xE2\x82\xAC c&gt;d</p>\n  <p>tail</p></div></body></html>";
  std::string expected;
  {
    XPathProgressResolver resolver(9, TargetMode::CodepointIndex);
    ASSERT_TRUE(resolver.ok());
    feed(resolver, xml, xml.size());
    ASSERT_TRUE(resolver.finish());
    ASSERT_TRUE(resolver.hasMatch());
    expected = resolver.getXPath();
  }
  ASSERT_FALSE(expected.empty());

  for (size_t chunk = 1; chunk <= xml.size(); chunk++) {
    XPathProgressResolver resolver(9, TargetMode::CodepointIndex);
    ASSERT_TRUE(resolver.ok());
    feed(resolver, xml, chunk);
    ASSERT_TRUE(resolver.finish()) << "chunk size " << chunk;
    ASSERT_TRUE(resolver.hasMatch()) << "chunk size " << chunk;
    EXPECT_EQ(resolver.getXPath(), expected) << "chunk size " << chunk;
  }
}

// --- XPathParagraphResolver --------------------------------------------------------

TEST(XPathParagraphResolver, ResolvesTheRealChapterShapeToTheNestedParagraph) {
  XPathParagraphResolver resolver(8);
  ASSERT_TRUE(resolver.ok());
  resolver.spineIndex = kRealSpineIndex;
  feed(resolver, realChapterShape(), 1024);
  ASSERT_TRUE(resolver.finish());
  ASSERT_TRUE(resolver.hasMatch());
  EXPECT_EQ(resolver.getXPath(), "/body/DocFragment[442]/body/div/div[2]/div/p[8]");
  EXPECT_EQ(resolver.getXPath().find("/body/p["), std::string::npos);
}

TEST(XPathParagraphResolver, OmitsTheIndexOnAUniquelyNamedSibling) {
  XPathParagraphResolver resolver(1);
  ASSERT_TRUE(resolver.ok());
  feed(resolver, "<html><body><section><p>only</p></section></body></html>", 9);
  ASSERT_TRUE(resolver.finish());
  ASSERT_TRUE(resolver.hasMatch());
  EXPECT_EQ(resolver.getXPath(), "/body/DocFragment[1]/body/section/p");
}

TEST(XPathParagraphResolver, CountsListItemsAsParagraphPositions) {
  XPathParagraphResolver resolver(2);
  ASSERT_TRUE(resolver.ok());
  feed(resolver, "<html><body><ul><li>a</li><li>b</li></ul></body></html>", 4);
  ASSERT_TRUE(resolver.finish());
  ASSERT_TRUE(resolver.hasMatch());
  EXPECT_EQ(resolver.getXPath(), "/body/DocFragment[1]/body/ul/li[2]");
}

// --- Fix 1: the last-resort shape must stay deleted --------------------------------

// ProgressMapper::generateXPath is the last resort when both expat passes return "".
// It cannot be exercised on the host (its signature takes a std::shared_ptr<Epub>,
// which drags in the SD HAL), so this pins the one property that matters as a source
// guard: the function must never again build "<base>/p[K]", a path that claims the
// paragraph is a direct child of the fragment body and therefore resolves to nothing
// in crengine for the great majority of real EPUBs.
TEST(ProgressMapperLastResort, GenerateXPathNeverBuildsADirectBodyParagraph) {
  const std::string src = readFile(CROSSPOINT_PROGRESS_MAPPER_CPP);
  ASSERT_FALSE(src.empty()) << "could not read " << CROSSPOINT_PROGRESS_MAPPER_CPP;

  const size_t start = src.find("std::string ProgressMapper::generateXPath");
  ASSERT_NE(start, std::string::npos);
  const std::string body = src.substr(start);

  EXPECT_EQ(body.find("\"/p[\""), std::string::npos)
      << "generateXPath must degrade to the top of the chapter, never to an unresolvable /p[K]";
  EXPECT_EQ(body.find("ParagraphStreamer"), std::string::npos)
      << "generateXPath no longer needs a paragraph count; the streamer should be gone with it";
}
