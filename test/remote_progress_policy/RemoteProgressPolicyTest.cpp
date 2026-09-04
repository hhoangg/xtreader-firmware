#include <gtest/gtest.h>

#include "RemoteProgressPolicy.h"

namespace {

using remote_progress_policy::chapterFromProgress;
using remote_progress_policy::decide;
using remote_progress_policy::Decision;
using remote_progress_policy::Input;
using remote_progress_policy::LEGACY_DEVICE_ID;
using remote_progress_policy::UNKNOWN_CHAPTER;

// A foreign device, in a different chapter than this one, and a row nobody has
// answered yet -- worth asking about, unless a test says otherwise.
Input foreignAndElsewhere() {
  Input in;
  in.haveRemote = true;
  in.remoteDeviceId = "kindle-abc";
  in.selfDeviceId = "dev_x4";
  in.remoteProgress = "/body/DocFragment[40]/body/div/p[3]/text().0";
  in.localSpineIndex = 7;
  in.remoteTimestamp = 1000;
  in.resolvedTimestamp = 0;
  return in;
}

// --- who wrote the row ---------------------------------------------------

TEST(RemoteProgressPolicy, ForeignDeviceInAnotherChapterPrompts) {
  EXPECT_EQ(decide(foreignAndElsewhere()), Decision::Prompt);
}

TEST(RemoteProgressPolicy, NothingStoredOnTheServerIsIgnored) {
  Input in = foreignAndElsewhere();
  in.haveRemote = false;
  EXPECT_EQ(decide(in), Decision::Ignore);
}

TEST(RemoteProgressPolicy, OurOwnUploadIsIgnored) {
  Input in = foreignAndElsewhere();
  in.remoteDeviceId = "dev_x4";
  EXPECT_EQ(decide(in), Decision::Ignore);
}

TEST(RemoteProgressPolicy, TheLegacyConstantCountsAsOurOwn) {
  // Every row this firmware wrote before per-device ids went on the wire
  // carries this value. Without this rule the device's own last upload
  // would prompt against itself on the very next open.
  Input in = foreignAndElsewhere();
  in.remoteDeviceId = LEGACY_DEVICE_ID;
  EXPECT_EQ(decide(in), Decision::Ignore);
}

TEST(RemoteProgressPolicy, AnUnidentifiedRowIsNotAssumedToBeOurs) {
  // A client that never identified itself is unknown, not self -- the reader
  // still deserves the choice.
  Input in = foreignAndElsewhere();
  in.remoteDeviceId = "";
  EXPECT_EQ(decide(in), Decision::Prompt);
}

TEST(RemoteProgressPolicy, AnUnpairedSelfIdStillTreatsTheLegacyConstantAsSelf) {
  Input in = foreignAndElsewhere();
  in.selfDeviceId = "";
  in.remoteDeviceId = LEGACY_DEVICE_ID;
  EXPECT_EQ(decide(in), Decision::Ignore);
}

// --- the chapter gate ----------------------------------------------------

TEST(RemoteProgressPolicy, TheSameChapterIsIgnored) {
  // DocFragment is 1-based, spine index is 0-based: fragment 8 is spine 7.
  Input in = foreignAndElsewhere();
  in.remoteProgress = "/body/DocFragment[8]/body/div/p[3]/text().0";
  in.localSpineIndex = 7;
  EXPECT_EQ(decide(in), Decision::Ignore);
}

TEST(RemoteProgressPolicy, TheOffByOneNeighbourIsADifferentChapter) {
  // Pins the conversion in the other direction: reading fragment 8 as spine 8
  // would silence a real move by one chapter.
  Input in = foreignAndElsewhere();
  in.remoteProgress = "/body/DocFragment[8]/body/div/p[3]/text().0";
  in.localSpineIndex = 8;
  EXPECT_EQ(decide(in), Decision::Prompt);
}

TEST(RemoteProgressPolicy, TheFirstChapterIsSpineZero) {
  Input in = foreignAndElsewhere();
  in.remoteProgress = "/body/DocFragment[1]/body/p[2]/text().0";
  in.localSpineIndex = 0;
  EXPECT_EQ(decide(in), Decision::Ignore);
}

TEST(RemoteProgressPolicy, ASingleFragmentEpubIsTheFirstChapter) {
  // No index at all, which a pattern requiring [N] would miss entirely.
  Input in = foreignAndElsewhere();
  in.remoteProgress = "/body/DocFragment/body/ul[2]/li[5]/text()[3].16";
  in.localSpineIndex = 0;
  EXPECT_EQ(decide(in), Decision::Ignore);
}

TEST(RemoteProgressPolicy, AChapterBehindUsStillPrompts) {
  // Reading on the other device does not have to move forward for the
  // question to be a real one -- it may be re-reading a chapter.
  Input in = foreignAndElsewhere();
  in.remoteProgress = "/body/DocFragment[2]/body/p[1]/text().0";
  in.localSpineIndex = 7;
  EXPECT_EQ(decide(in), Decision::Prompt);
}

TEST(RemoteProgressPolicy, TheSameChapterIsIgnoredWhereverInItEachSideIs) {
  // The production row that exposed the percentage bug: KOReader said 22.79%
  // of its own page count, this device said 24% of the book's bytes, and the
  // two are not the same quantity. Same chapter, so no dialog.
  Input in = foreignAndElsewhere();
  in.remoteProgress = "/body/DocFragment[440]/body/div/div[1]/h2/text().0";
  in.localSpineIndex = 439;
  EXPECT_EQ(decide(in), Decision::Ignore);
}

TEST(RemoteProgressPolicy, AnUnreadableChapterPrompts) {
  // Bias to asking: a missed prompt strands the reader somewhere they cannot
  // explain, an extra one costs a button press.
  Input in = foreignAndElsewhere();
  in.remoteProgress = "/FictionBook/body/section[3]/p[7]/text().12";
  EXPECT_EQ(decide(in), Decision::Prompt);
}

TEST(RemoteProgressPolicy, AnEmptyProgressStringPrompts) {
  Input in = foreignAndElsewhere();
  in.remoteProgress = "";
  EXPECT_EQ(decide(in), Decision::Prompt);
}

// --- remembering that this row was already answered ----------------------

TEST(RemoteProgressPolicy, ARowAlreadyAnsweredIsIgnored) {
  // The other half of the bug: declining used to be forgotten the moment the
  // book closed, so the same row asked again on every open.
  Input in = foreignAndElsewhere();
  in.remoteTimestamp = 1000;
  in.resolvedTimestamp = 1000;
  EXPECT_EQ(decide(in), Decision::Ignore);
}

TEST(RemoteProgressPolicy, ARowOlderThanTheAnsweredOneIsIgnored) {
  Input in = foreignAndElsewhere();
  in.remoteTimestamp = 900;
  in.resolvedTimestamp = 1000;
  EXPECT_EQ(decide(in), Decision::Ignore);
}

TEST(RemoteProgressPolicy, AGenuinelyNewerRowStillPrompts) {
  Input in = foreignAndElsewhere();
  in.remoteTimestamp = 1001;
  in.resolvedTimestamp = 1000;
  EXPECT_EQ(decide(in), Decision::Prompt);
}

TEST(RemoteProgressPolicy, ABookNeverAnsweredAboutPrompts) {
  Input in = foreignAndElsewhere();
  in.remoteTimestamp = 1000;
  in.resolvedTimestamp = 0;
  EXPECT_EQ(decide(in), Decision::Prompt);
}

TEST(RemoteProgressPolicy, AnUnstampedRowFallsThroughToTheChapterGate) {
  // Nothing to key on, so the marker cannot settle it either way. The chapter
  // decides, exactly as if no marker existed.
  Input in = foreignAndElsewhere();
  in.remoteTimestamp = 0;
  in.resolvedTimestamp = 1000;
  EXPECT_EQ(decide(in), Decision::Prompt);

  in.remoteProgress = "/body/DocFragment[8]/body/p[1]/text().0";
  in.localSpineIndex = 7;
  EXPECT_EQ(decide(in), Decision::Ignore);
}

TEST(RemoteProgressPolicy, AnAnsweredRowIsIgnoredEvenWithAnUnreadableChapter) {
  Input in = foreignAndElsewhere();
  in.remoteProgress = "512";
  in.remoteTimestamp = 1000;
  in.resolvedTimestamp = 1000;
  EXPECT_EQ(decide(in), Decision::Ignore);
}

// --- chapterFromProgress -------------------------------------------------

TEST(ChapterFromProgress, IndexedDocFragmentIsItsOwnNumber) {
  EXPECT_EQ(chapterFromProgress("/body/DocFragment[440]/body/div/div[1]/h2/text().0"), 440);
  EXPECT_EQ(chapterFromProgress("/body/DocFragment[1]/body/p[2]/text().17"), 1);
  EXPECT_EQ(chapterFromProgress("/body/DocFragment[8]"), 8);
}

TEST(ChapterFromProgress, AMissingIndexMeansTheOnlyFragment) {
  EXPECT_EQ(chapterFromProgress("/body/DocFragment/body/ul[2]/li[5]/text()[3].16"), 1);
  EXPECT_EQ(chapterFromProgress("/body/DocFragment"), 1);
  EXPECT_EQ(chapterFromProgress("/body/DocFragment.0"), 1);
}

TEST(ChapterFromProgress, Fb2NamesNoFragment) {
  EXPECT_EQ(chapterFromProgress("/FictionBook/body/section[3]/p[7]/text().12"), UNKNOWN_CHAPTER);
}

TEST(ChapterFromProgress, HtmlAndTxtNameNoFragment) {
  EXPECT_EQ(chapterFromProgress("/html/body/div/p[3]/text().0"), UNKNOWN_CHAPTER);
  EXPECT_EQ(chapterFromProgress("/html[2]/body/p[9]/text().4"), UNKNOWN_CHAPTER);
}

TEST(ChapterFromProgress, APagedFormatSendsAPageNumberNotAPath) {
  // ReaderPaging::getLastProgress returns getTopPage() for PDF/DJVU/CBZ, and
  // kosync sends that integer in the same field.
  EXPECT_EQ(chapterFromProgress("173"), UNKNOWN_CHAPTER);
  EXPECT_EQ(chapterFromProgress("0"), UNKNOWN_CHAPTER);
}

TEST(ChapterFromProgress, EmptyIsUnknown) { EXPECT_EQ(chapterFromProgress(""), UNKNOWN_CHAPTER); }

TEST(ChapterFromProgress, AMalformedIndexIsUnknownRatherThanAGuess) {
  EXPECT_EQ(chapterFromProgress("/body/DocFragment[0]/body/p[1]"), UNKNOWN_CHAPTER);
  EXPECT_EQ(chapterFromProgress("/body/DocFragment[]/body"), UNKNOWN_CHAPTER);
  EXPECT_EQ(chapterFromProgress("/body/DocFragment[12/body"), UNKNOWN_CHAPTER);
  EXPECT_EQ(chapterFromProgress("/body/DocFragment[abc]/body"), UNKNOWN_CHAPTER);
  EXPECT_EQ(chapterFromProgress("/body/DocFragment[3x]/body"), UNKNOWN_CHAPTER);
  EXPECT_EQ(chapterFromProgress("/body/DocFragment["), UNKNOWN_CHAPTER);
}

TEST(ChapterFromProgress, AnIndexTooLargeToBeASpineIsUnknown) {
  EXPECT_EQ(chapterFromProgress("/body/DocFragment[999999999999]/body/p[1]"), UNKNOWN_CHAPTER);
}

TEST(ChapterFromProgress, TheFragmentMustBeWhereKoreaderPutsIt) {
  // A prefix match anywhere in the string would accept paths that mean
  // something else entirely.
  EXPECT_EQ(chapterFromProgress("/foo/body/DocFragment[3]/body"), UNKNOWN_CHAPTER);
  EXPECT_EQ(chapterFromProgress("/body/DocFragments[3]/body"), UNKNOWN_CHAPTER);
  EXPECT_EQ(chapterFromProgress("body/DocFragment[3]/body"), UNKNOWN_CHAPTER);
  EXPECT_EQ(chapterFromProgress("/body/DocFrag"), UNKNOWN_CHAPTER);
}

}  // namespace
