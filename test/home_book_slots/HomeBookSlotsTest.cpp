#include <gtest/gtest.h>

#include "HomeBookSlots.h"

namespace {

using home_book_slots::DownloadedCandidate;
using home_book_slots::fill;
using home_book_slots::Input;
using home_book_slots::QueueView;
using home_book_slots::RecentCandidate;
using home_book_slots::RemoteCandidate;
using home_book_slots::Slot;
using home_book_slots::SLOT_COUNT;
using home_book_slots::State;

RemoteCandidate remote(const std::string& id, const std::string& path, uint64_t updatedAt) {
  RemoteCandidate r;
  r.id = id;
  r.path = path;
  r.sizeBytes = 1000;
  r.updatedAt = updatedAt;
  return r;
}

DownloadedCandidate downloaded(const std::string& path, uint64_t sizeBytes = 0) {
  DownloadedCandidate d;
  d.path = path;
  d.sizeBytes = sizeBytes;
  return d;
}

RecentCandidate recent(const std::string& path, const std::string& title) {
  RecentCandidate r;
  r.path = path;
  r.title = title;
  r.author = "Author";
  r.coverBmpPath = path + ".cover.bmp";
  r.progressPercent = 42;
  return r;
}

TEST(HomeBookSlots, EmptyInputYieldsNoSlots) {
  Input in;
  EXPECT_TRUE(fill(in).empty());
}

TEST(HomeBookSlots, OneRemotePlusTwoRecentsFillsInThatOrder) {
  Input in;
  in.remote = {remote("r1", "/books/r1.epub", 100)};
  in.recents = {recent("/books/a.epub", "A"), recent("/books/b.epub", "B")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 3u);

  EXPECT_TRUE(slots[0].remote);
  EXPECT_EQ(slots[0].id, "r1");
  EXPECT_EQ(slots[0].state, State::OnServer);

  EXPECT_FALSE(slots[1].remote);
  EXPECT_EQ(slots[1].path, "/books/a.epub");
  EXPECT_EQ(slots[1].state, State::Read);

  EXPECT_FALSE(slots[2].remote);
  EXPECT_EQ(slots[2].path, "/books/b.epub");
}

TEST(HomeBookSlots, RemoteSlotCarriesSizeBytesFromTheCandidate) {
  Input in;
  RemoteCandidate r = remote("r1", "/books/r1.epub", 100);
  r.sizeBytes = 123456;
  in.remote = {r};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].sizeBytes, 123456u);
}

TEST(HomeBookSlots, RecentSlotSizeBytesStaysZero) {
  Input in;
  in.recents = {recent("/books/a.epub", "A")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].sizeBytes, 0u);
}

TEST(HomeBookSlots, RemoteTitleAndAuthorAreDerivedFromThePath) {
  Input in;
  in.remote = {remote("r1", "/Uncollected/Tam The.epub", 100)};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].title, "Tam The");
  EXPECT_EQ(slots[0].author, "Uncollected");
}

TEST(HomeBookSlots, RemoteTitleWithNoDirectoryComponent) {
  Input in;
  in.remote = {remote("r1", "Tam The.epub", 100)};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].title, "Tam The");
  EXPECT_EQ(slots[0].author, "");
}

TEST(HomeBookSlots, RemoteTitleWithNoExtension) {
  Input in;
  in.remote = {remote("r1", "/books/Tam The", 100)};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].title, "Tam The");
  EXPECT_EQ(slots[0].author, "books");
}

TEST(HomeBookSlots, RemoteTitleWithTrailingDot) {
  Input in;
  in.remote = {remote("r1", "/books/Tam The.", 100)};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].title, "Tam The");
  EXPECT_EQ(slots[0].author, "books");
}

TEST(HomeBookSlots, RemoteTitleAtRootGivesEmptyAuthor) {
  Input in;
  in.remote = {remote("r1", "/Tam The.epub", 100)};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].title, "Tam The");
  EXPECT_EQ(slots[0].author, "");
}

TEST(HomeBookSlots, RemoteTitleUsesTheInnermostFolderForNestedPaths) {
  Input in;
  in.remote = {remote("r1", "/Library/Fiction/Uncollected/Tam The.epub", 100)};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].title, "Tam The");
  EXPECT_EQ(slots[0].author, "Uncollected");
}

TEST(HomeBookSlots, FourRemoteBooksFillAllThreeSlotsAndNoRecentAppears) {
  Input in;
  in.remote = {
      remote("r1", "/books/r1.epub", 400),
      remote("r2", "/books/r2.epub", 300),
      remote("r3", "/books/r3.epub", 200),
      remote("r4", "/books/r4.epub", 100),
  };
  in.recents = {recent("/books/a.epub", "A")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), SLOT_COUNT);
  for (const Slot& s : slots) {
    EXPECT_TRUE(s.remote);
  }
  EXPECT_EQ(slots[0].id, "r1");
  EXPECT_EQ(slots[1].id, "r2");
  EXPECT_EQ(slots[2].id, "r3");
}

TEST(HomeBookSlots, RecentEqualToACoverTilePathIsSkipped) {
  Input in;
  in.coverTilePaths = {"/books/a.epub"};
  in.recents = {recent("/books/a.epub", "A"), recent("/books/b.epub", "B")};

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
  in.recents = {recent("/books/a.epub", "A"), recent("/books/b.epub", "B"), recent("/books/c.epub", "C"),
                recent("/books/d.epub", "D")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].path, "/books/d.epub");
}

TEST(HomeBookSlots, NoCoverTilePathsSkipsNothing) {
  Input in;
  in.recents = {recent("/books/a.epub", "A"), recent("/books/b.epub", "B")};

  EXPECT_EQ(fill(in).size(), 2u);
}

TEST(HomeBookSlots, RecentDuplicatingAnAlreadyPlacedRemoteIsSkipped) {
  // The same path showing up on both sides (re-uploaded, or a stale recents
  // entry) must not produce two slots for one book.
  Input in;
  in.remote = {remote("r1", "/books/shared.epub", 100)};
  in.recents = {recent("/books/shared.epub", "Shared"), recent("/books/b.epub", "B")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 2u);
  EXPECT_TRUE(slots[0].remote);
  EXPECT_EQ(slots[0].path, "/books/shared.epub");
  EXPECT_FALSE(slots[1].remote);
  EXPECT_EQ(slots[1].path, "/books/b.epub");
}

TEST(HomeBookSlots, RemoteBookAlsoQueuedReportsQueuedWithPosition) {
  Input in;
  in.remote = {remote("r1", "/books/r1.epub", 100)};
  in.queue.entries = {{"r1", State::Queued, 2}};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].state, State::Queued);
  EXPECT_EQ(slots[0].queuePosition, 2);
}

TEST(HomeBookSlots, RemoteBookActivelyDownloadingReportsDownloading) {
  Input in;
  in.remote = {remote("r1", "/books/r1.epub", 100)};
  in.queue.entries = {{"r1", State::Downloading, 0}};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].state, State::Downloading);
}

TEST(HomeBookSlots, LastFailedIdProducesFailed) {
  Input in;
  in.remote = {remote("r1", "/books/r1.epub", 100)};
  in.queue.lastFailedId = "r1";

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].state, State::Failed);
}

TEST(HomeBookSlots, QueuedOutranksFailedForSameBook) {
  // A retry that re-queues a previously failed id must show as Queued, not
  // resurface the old failure.
  Input in;
  in.remote = {remote("r1", "/books/r1.epub", 100)};
  in.queue.entries = {{"r1", State::Queued, 1}};
  in.queue.lastFailedId = "r1";

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].state, State::Queued);
  EXPECT_EQ(slots[0].queuePosition, 1);
}

TEST(HomeBookSlots, TiesOnUpdatedAtSortByPath) {
  Input in;
  in.remote = {
      remote("r1", "/books/zzz.epub", 100),
      remote("r2", "/books/aaa.epub", 100),
  };

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 2u);
  EXPECT_EQ(slots[0].path, "/books/aaa.epub");
  EXPECT_EQ(slots[1].path, "/books/zzz.epub");
}

TEST(HomeBookSlots, JustDownloadedAppearsWhenItIsInNeitherOtherSource) {
  // The gap this source exists to close: markDownloaded() drops the book from
  // the manifest scan, and it only reaches recents once the reader opens it.
  Input in;
  in.justDownloaded = {downloaded("/novels/Fresh Book.epub", 2048)};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_FALSE(slots[0].remote);
  EXPECT_EQ(slots[0].path, "/novels/Fresh Book.epub");
  EXPECT_EQ(slots[0].state, State::JustDownloaded);
  EXPECT_EQ(slots[0].sizeBytes, 2048u);
}

TEST(HomeBookSlots, JustDownloadedTitleAndAuthorComeFromThePath) {
  // The file is local now but has never been opened, so there is no parsed
  // title and no cover -- same derivation the remote branch uses.
  Input in;
  in.justDownloaded = {downloaded("/novels/Verne/Around the World.epub")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].title, "Around the World");
  EXPECT_EQ(slots[0].author, "Verne");
  EXPECT_TRUE(slots[0].coverBmpPath.empty());
}

TEST(HomeBookSlots, JustDownloadedSortsAfterRemoteAndBeforeRecents) {
  Input in;
  in.remote = {remote("r1", "/books/r1.epub", 100)};
  in.justDownloaded = {downloaded("/books/fresh.epub")};
  in.recents = {recent("/books/old.epub", "Old")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 3u);
  EXPECT_EQ(slots[0].path, "/books/r1.epub");
  EXPECT_EQ(slots[1].path, "/books/fresh.epub");
  EXPECT_EQ(slots[2].path, "/books/old.epub");
}

TEST(HomeBookSlots, ThreeRemoteBooksPushJustDownloadedOutEntirely) {
  // Deliberate: remote outranks everything and there is no per-source cap, so
  // a busy server can hide a book that just landed. Pinned so it is not
  // "fixed" into a reserved slot by accident.
  Input in;
  in.remote = {remote("r1", "/books/r1.epub", 300), remote("r2", "/books/r2.epub", 200),
               remote("r3", "/books/r3.epub", 100)};
  in.justDownloaded = {downloaded("/books/fresh.epub")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), SLOT_COUNT);
  for (const Slot& slot : slots) {
    EXPECT_TRUE(slot.remote);
  }
}

TEST(HomeBookSlots, JustDownloadedAlsoInRecentsIsRetiredToAnOrdinaryReadRow) {
  // Presence in recents means the reader has opened it, so the badge has done
  // its job. Without this the row stays "NEW / Not started" forever, even at
  // 40% read, and three downloads pin all three rows permanently.
  Input in;
  in.justDownloaded = {downloaded("/books/fresh.epub")};
  in.recents = {recent("/books/fresh.epub", "Fresh")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].state, State::Read);
  // ...and it is the recents entry, with the title and cover opening it gave us.
  EXPECT_EQ(slots[0].title, "Fresh");
  EXPECT_FALSE(slots[0].coverBmpPath.empty());
  EXPECT_EQ(slots[0].progressPercent, 42);
}

TEST(HomeBookSlots, JustDownloadedStaysNewWhileItIsNotInRecents) {
  Input in;
  in.justDownloaded = {downloaded("/books/fresh.epub")};
  in.recents = {recent("/books/other.epub", "Other")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 2u);
  EXPECT_EQ(slots[0].path, "/books/fresh.epub");
  EXPECT_EQ(slots[0].state, State::JustDownloaded);
  EXPECT_EQ(slots[1].state, State::Read);
}

TEST(HomeBookSlots, JustDownloadedIsSkippedWhenItIsTheCoverTilesOwnBook) {
  Input in;
  in.justDownloaded = {downloaded("/books/tile.epub")};
  in.coverTilePaths = {"/books/tile.epub"};

  EXPECT_TRUE(fill(in).empty());
}

TEST(HomeBookSlots, JustDownloadedIsSkippedWhenTheManifestStillListsIt) {
  // A download that finished but whose markDownloaded() write failed: the
  // remote entry wins, so the row shows the server state rather than twice.
  Input in;
  in.remote = {remote("r1", "/books/fresh.epub", 100)};
  in.justDownloaded = {downloaded("/books/fresh.epub")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_TRUE(slots[0].remote);
}

TEST(HomeBookSlots, RecentNotInJustDownloadedIsRead) {
  Input in;
  in.recents = {recent("/books/old.epub", "Old")};

  const std::vector<Slot> slots = fill(in);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots[0].state, State::Read);
}

}  // namespace
