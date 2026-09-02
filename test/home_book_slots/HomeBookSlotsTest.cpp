#include <gtest/gtest.h>

#include "HomeBookSlots.h"

namespace {

using home_book_slots::fill;
using home_book_slots::Input;
using home_book_slots::QueueView;
using home_book_slots::RecentCandidate;
using home_book_slots::Slot;
using home_book_slots::SLOT_COUNT;
using home_book_slots::State;

// A book the server has that this device does not: no title, no author, no
// cover, no progress -- nothing here has ever opened it.
RecentCandidate remote(const std::string& id, const std::string& path, uint64_t sizeBytes = 1000) {
  RecentCandidate r;
  r.path = path;
  r.remoteId = id;
  r.sizeBytes = sizeBytes;
  return r;
}

// A book on the SD card that has been read: it has a cached percentage.
RecentCandidate local(const std::string& path, const std::string& title, int progressPercent = 42) {
  RecentCandidate r;
  r.path = path;
  r.title = title;
  r.author = "Author";
  r.coverBmpPath = path + ".cover.bmp";
  r.progressPercent = progressPercent;
  return r;
}

TEST(HomeBookSlots, EmptyInputYieldsNoSlots) {
  Input in;
  EXPECT_TRUE(fill(in).empty());
}

// --- The whole ordering rule ------------------------------------------------

TEST(HomeBookSlots, SlotsFollowTheListsOwnOrderRegardlessOfKind) {
  // The feature, in one assertion: a book opened after a new one was
  // discovered sits above it, because the list is the sequence and kind does
  // not outrank it.
  Input in;
  in.recents = {local("/books/b2.epub", "B2"), remote("r4", "/books/b4.epub"), local("/books/b3.epub", "B3")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 3u);
  EXPECT_EQ(slots[0].path, "/books/b2.epub");
  EXPECT_FALSE(slots[0].remote);
  EXPECT_EQ(slots[1].path, "/books/b4.epub");
  EXPECT_TRUE(slots[1].remote);
  EXPECT_EQ(slots[2].path, "/books/b3.epub");
  EXPECT_FALSE(slots[2].remote);
}

TEST(HomeBookSlots, NothingIsReordered) {
  // Not even among remote entries: whatever discovery decided when it
  // inserted them is the order, and fill() has no sort of its own to
  // second-guess it with.
  Input in;
  in.recents = {remote("r1", "/books/zzz.epub"), remote("r2", "/books/aaa.epub")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 2u);
  EXPECT_EQ(slots[0].path, "/books/zzz.epub");
  EXPECT_EQ(slots[1].path, "/books/aaa.epub");
}

TEST(HomeBookSlots, StopsAtSlotCountAndTheTailIsDropped) {
  Input in;
  in.recents = {local("/books/a.epub", "A"), remote("r1", "/books/r1.epub"), local("/books/b.epub", "B"),
                local("/books/c.epub", "C")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), SLOT_COUNT);
  EXPECT_EQ(slots[2].path, "/books/b.epub");
}

// --- The cover tile skip ----------------------------------------------------

TEST(HomeBookSlots, EntryEqualToACoverTilePathIsSkipped) {
  Input in;
  in.coverTilePaths = {"/books/a.epub"};
  in.recents = {local("/books/a.epub", "A"), local("/books/b.epub", "B")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].path, "/books/b.epub");
}

TEST(HomeBookSlots, EveryCoverTilePathIsSkippedNotJustTheFirst) {
  // Lyra3Covers puts three books in the tile band. With a single-path tile
  // field the second and third appeared again as rows, so one screen showed
  // the same book twice with two selector positions.
  Input in;
  in.coverTilePaths = {"/books/a.epub", "/books/b.epub", "/books/c.epub"};
  in.recents = {local("/books/a.epub", "A"), local("/books/b.epub", "B"), local("/books/c.epub", "C"),
                local("/books/d.epub", "D")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].path, "/books/d.epub");
}

TEST(HomeBookSlots, SkippingACoverTileEntryLetsALaterEntryTakeItsSlot) {
  // The skip must not cost a slot: three tile books plus four list entries
  // still fills all three rows.
  Input in;
  in.coverTilePaths = {"/books/a.epub"};
  in.recents = {local("/books/a.epub", "A"), local("/books/b.epub", "B"), local("/books/c.epub", "C"),
                local("/books/d.epub", "D")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), SLOT_COUNT);
  EXPECT_EQ(slots[2].path, "/books/d.epub");
}

TEST(HomeBookSlots, NoCoverTilePathsSkipsNothing) {
  Input in;
  in.recents = {local("/books/a.epub", "A"), local("/books/b.epub", "B")};

  EXPECT_EQ(fill(in).size(), 2u);
}

// --- The six row states -----------------------------------------------------

TEST(HomeBookSlots, RemoteEntryNotInTheQueueIsOnServer) {
  Input in;
  in.recents = {remote("r1", "/books/r1.epub", 123456)};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_TRUE(slots[0].remote);
  EXPECT_EQ(slots[0].id, "r1");
  EXPECT_EQ(slots[0].state, State::OnServer);
  EXPECT_EQ(slots[0].sizeBytes, 123456u);
  EXPECT_TRUE(slots[0].coverBmpPath.empty());
}

TEST(HomeBookSlots, RemoteEntryAlsoQueuedReportsQueuedWithPosition) {
  Input in;
  in.recents = {remote("r1", "/books/r1.epub")};
  in.queue.entries = {{"r1", State::Queued, 2}};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].state, State::Queued);
  EXPECT_EQ(slots[0].queuePosition, 2);
}

TEST(HomeBookSlots, RemoteEntryActivelyDownloadingReportsDownloading) {
  Input in;
  in.recents = {remote("r1", "/books/r1.epub")};
  in.queue.entries = {{"r1", State::Downloading, 0}};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].state, State::Downloading);
}

TEST(HomeBookSlots, LastFailedIdProducesFailed) {
  Input in;
  in.recents = {remote("r1", "/books/r1.epub")};
  in.queue.lastFailedId = "r1";

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].state, State::Failed);
}

TEST(HomeBookSlots, QueuedOutranksFailedForSameBook) {
  // A retry that re-queues a previously failed id must show as Queued, not
  // resurface the old failure.
  Input in;
  in.recents = {remote("r1", "/books/r1.epub")};
  in.queue.entries = {{"r1", State::Queued, 1}};
  in.queue.lastFailedId = "r1";

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].state, State::Queued);
  EXPECT_EQ(slots[0].queuePosition, 1);
}

TEST(HomeBookSlots, ALocalEntryWithNoCachedPercentageIsJustDownloaded) {
  // What a finished download leaves behind: markDownloaded() clears remoteId
  // in place, and nothing has opened the book yet, so it wears the NEW badge.
  Input in;
  RecentCandidate landed;
  landed.path = "/novels/Fresh Book.epub";
  in.recents = {landed};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_FALSE(slots[0].remote);
  EXPECT_EQ(slots[0].state, State::JustDownloaded);
  EXPECT_TRUE(slots[0].id.empty());
}

TEST(HomeBookSlots, ALocalEntryRetiresToReadAsSoonAsItHasAPercentage) {
  // Every close of a book writes one, including a close at 0%, so the badge
  // cannot outlive the first read.
  Input in;
  in.recents = {local("/books/fresh.epub", "Fresh", 0), local("/books/old.epub", "Old", 40)};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 2u);
  EXPECT_EQ(slots[0].state, State::Read);
  EXPECT_EQ(slots[1].state, State::Read);
  EXPECT_EQ(slots[1].progressPercent, 40);
}

TEST(HomeBookSlots, ALocalEntryKeepsTheMetadataOpeningItGaveUs) {
  Input in;
  in.recents = {local("/books/a.epub", "A")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].title, "A");
  EXPECT_EQ(slots[0].author, "Author");
  EXPECT_FALSE(slots[0].coverBmpPath.empty());
  EXPECT_EQ(slots[0].progressPercent, 42);
  EXPECT_EQ(slots[0].sizeBytes, 0u);
}

// --- A remote entry's title and author come from its path -------------------

TEST(HomeBookSlots, RemoteTitleAndAuthorAreDerivedFromThePath) {
  Input in;
  in.recents = {remote("r1", "/Uncollected/Tam The.epub")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].title, "Tam The");
  EXPECT_EQ(slots[0].author, "Uncollected");
}

TEST(HomeBookSlots, RemoteTitleWithNoDirectoryComponent) {
  Input in;
  in.recents = {remote("r1", "Tam The.epub")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].title, "Tam The");
  EXPECT_EQ(slots[0].author, "");
}

TEST(HomeBookSlots, RemoteTitleWithNoExtension) {
  Input in;
  in.recents = {remote("r1", "/books/Tam The")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].title, "Tam The");
  EXPECT_EQ(slots[0].author, "books");
}

TEST(HomeBookSlots, RemoteTitleWithTrailingDot) {
  Input in;
  in.recents = {remote("r1", "/books/Tam The.")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].title, "Tam The");
  EXPECT_EQ(slots[0].author, "books");
}

TEST(HomeBookSlots, RemoteTitleAtRootGivesEmptyAuthor) {
  Input in;
  in.recents = {remote("r1", "/Tam The.epub")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].title, "Tam The");
  EXPECT_EQ(slots[0].author, "");
}

TEST(HomeBookSlots, RemoteTitleUsesTheInnermostFolderForNestedPaths) {
  Input in;
  in.recents = {remote("r1", "/Library/Fiction/Uncollected/Tam The.epub")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].title, "Tam The");
  EXPECT_EQ(slots[0].author, "Uncollected");
}

}  // namespace
