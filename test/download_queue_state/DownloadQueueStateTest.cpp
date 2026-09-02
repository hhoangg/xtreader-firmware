#include <gtest/gtest.h>

#include "QueueState.h"

namespace {

using download_queue::ItemStatus;
using download_queue::MAX_QUEUE;
using download_queue::QueueItem;
using download_queue::QueueState;

TEST(QueueStateEnqueue, StartsEmpty) {
  QueueState q;
  EXPECT_TRUE(q.empty());
  EXPECT_EQ(q.size(), 0u);
  EXPECT_EQ(q.front(), nullptr);
}

TEST(QueueStateEnqueue, AddsItemsInOrder) {
  QueueState q;
  ASSERT_EQ(q.enqueue("bok_1", "/a.epub", 100), QueueState::EnqueueResult::Ok);
  ASSERT_EQ(q.enqueue("bok_2", "/b.epub", 200), QueueState::EnqueueResult::Ok);
  ASSERT_EQ(q.enqueue("bok_3", "/c.epub", 300), QueueState::EnqueueResult::Ok);

  EXPECT_EQ(q.size(), 3u);
  ASSERT_NE(q.front(), nullptr);
  EXPECT_EQ(q.front()->id, "bok_1");
  EXPECT_EQ(q.front()->status, ItemStatus::Pending);
}

TEST(QueueStateEnqueue, RejectsADuplicateIdAlreadyQueued) {
  QueueState q;
  ASSERT_EQ(q.enqueue("bok_1", "/a.epub", 100), QueueState::EnqueueResult::Ok);
  EXPECT_EQ(q.enqueue("bok_1", "/a.epub", 100), QueueState::EnqueueResult::AlreadyQueued);
  EXPECT_EQ(q.size(), 1u);
}

TEST(QueueStateEnqueue, EnforcesTheBound) {
  QueueState q;
  for (size_t i = 0; i < MAX_QUEUE; i++) {
    ASSERT_EQ(q.enqueue("bok_" + std::to_string(i), "/x.epub", 1), QueueState::EnqueueResult::Ok);
  }
  EXPECT_EQ(q.size(), MAX_QUEUE);
  EXPECT_EQ(q.enqueue("bok_overflow", "/x.epub", 1), QueueState::EnqueueResult::Full);
  EXPECT_EQ(q.size(), MAX_QUEUE);
}

TEST(QueueStateProcessing, MarkDownloadingFlipsOnlyTheFrontItem) {
  QueueState q;
  q.enqueue("bok_1", "/a.epub", 100);
  q.enqueue("bok_2", "/b.epub", 200);

  q.markDownloading("bok_1");
  EXPECT_EQ(q.front()->status, ItemStatus::Downloading);

  QueueItem items[MAX_QUEUE];
  const size_t n = q.copyItemsTo(items, MAX_QUEUE);
  ASSERT_EQ(n, 2u);
  EXPECT_EQ(items[1].status, ItemStatus::Pending);  // second item untouched
}

TEST(QueueStateProcessing, MarkDownloadingIsANoOpForAnIdThatIsNotTheFront) {
  QueueState q;
  q.enqueue("bok_1", "/a.epub", 100);
  q.enqueue("bok_2", "/b.epub", 200);

  q.markDownloading("bok_2");  // stale/racing caller -- bok_2 is not the front
  EXPECT_EQ(q.front()->status, ItemStatus::Pending);
}

TEST(QueueStateProcessing, UpdateProgressUpdatesTheFrontItemsCounters) {
  QueueState q;
  q.enqueue("bok_1", "/a.epub", 1000);
  q.updateProgress("bok_1", 400, 1000);

  ASSERT_NE(q.front(), nullptr);
  EXPECT_EQ(q.front()->downloadedBytes, 400u);
  EXPECT_EQ(q.front()->totalBytes, 1000u);
}

TEST(QueueStateProcessing, UpdateProgressIsANoOpOnAnEmptyQueue) {
  QueueState q;
  q.updateProgress("bok_1", 400, 1000);  // must not crash or resurrect anything
  EXPECT_TRUE(q.empty());
}

TEST(QueueStateProcessing, ASuccessfulFinishAdvancesToTheNextItem) {
  QueueState q;
  q.enqueue("bok_1", "/a.epub", 100);
  q.enqueue("bok_2", "/b.epub", 200);

  q.finish("bok_1", true, "");

  EXPECT_EQ(q.size(), 1u);
  ASSERT_NE(q.front(), nullptr);
  EXPECT_EQ(q.front()->id, "bok_2");
  EXPECT_TRUE(q.lastResult().hasResult);
  EXPECT_TRUE(q.lastResult().ok);
  EXPECT_EQ(q.lastResult().id, "bok_1");
}

TEST(QueueStateProcessing, AFailureAdvancesRatherThanHaltingTheQueue) {
  QueueState q;
  q.enqueue("bok_1", "/a.epub", 100);
  q.enqueue("bok_2", "/b.epub", 200);
  q.enqueue("bok_3", "/c.epub", 300);

  q.finish("bok_1", false, "fetch_failed");
  ASSERT_NE(q.front(), nullptr);
  EXPECT_EQ(q.front()->id, "bok_2");
  EXPECT_FALSE(q.lastResult().ok);
  EXPECT_EQ(q.lastResult().error, "fetch_failed");

  // The rest of the queue is untouched and keeps processing normally.
  q.finish("bok_2", true, "");
  ASSERT_NE(q.front(), nullptr);
  EXPECT_EQ(q.front()->id, "bok_3");
}

TEST(QueueStateProcessing, FinishIsANoOpForAnIdThatIsNotTheFront) {
  QueueState q;
  q.enqueue("bok_1", "/a.epub", 100);
  q.enqueue("bok_2", "/b.epub", 200);

  // Simulates a result arriving for a book cancelAll() already dropped, racing
  // against a newer front -- must not corrupt or remove the wrong item.
  q.finish("bok_2", true, "");
  EXPECT_EQ(q.size(), 2u);
  EXPECT_EQ(q.front()->id, "bok_1");
}

TEST(QueueStateProcessing, FinishOnAnEmptyQueueIsANoOp) {
  QueueState q;
  q.finish("bok_1", true, "");
  EXPECT_TRUE(q.empty());
  EXPECT_FALSE(q.lastResult().hasResult);
}

TEST(QueueStateCancel, CancelAllEmptiesTheQueue) {
  QueueState q;
  q.enqueue("bok_1", "/a.epub", 100);
  q.enqueue("bok_2", "/b.epub", 200);
  q.markDownloading("bok_1");

  q.cancelAll();

  EXPECT_TRUE(q.empty());
  EXPECT_EQ(q.size(), 0u);
  EXPECT_EQ(q.front(), nullptr);
}

TEST(QueueStateCancel, CancelAllOnAnEmptyQueueIsSafe) {
  QueueState q;
  q.cancelAll();
  EXPECT_TRUE(q.empty());
}

TEST(QueueStateCancel, QueueAcceptsNewWorkAfterACancel) {
  QueueState q;
  q.enqueue("bok_1", "/a.epub", 100);
  q.cancelAll();

  ASSERT_EQ(q.enqueue("bok_2", "/b.epub", 200), QueueState::EnqueueResult::Ok);
  EXPECT_EQ(q.size(), 1u);
  EXPECT_EQ(q.front()->id, "bok_2");
}

TEST(QueueStateSnapshot, CopyItemsToCapsAtMaxOut) {
  QueueState q;
  for (size_t i = 0; i < MAX_QUEUE; i++) {
    q.enqueue("bok_" + std::to_string(i), "/x.epub", 1);
  }

  QueueItem items[3];
  const size_t n = q.copyItemsTo(items, 3);
  EXPECT_EQ(n, 3u);
  EXPECT_EQ(items[0].id, "bok_0");
  EXPECT_EQ(items[2].id, "bok_2");
}

// The two monotonic counters FileBrowserActivity::pollDownloadQueue() polls
// instead of taking a full snapshot() every tick.

TEST(QueueStatePulse, StartsAtZero) {
  QueueState q;
  EXPECT_EQ(q.generation(), 0u);
  EXPECT_EQ(q.completions(), 0u);
}

TEST(QueueStatePulse, GenerationAdvancesOnAnAcceptedEnqueue) {
  QueueState q;
  const uint32_t before = q.generation();
  ASSERT_EQ(q.enqueue("bok_1", "/a.epub", 100), QueueState::EnqueueResult::Ok);
  EXPECT_NE(q.generation(), before);
}

TEST(QueueStatePulse, GenerationAdvancesOnMarkDownloading) {
  QueueState q;
  q.enqueue("bok_1", "/a.epub", 100);
  const uint32_t before = q.generation();
  q.markDownloading("bok_1");
  EXPECT_NE(q.generation(), before);
}

TEST(QueueStatePulse, GenerationAdvancesOnFinish) {
  QueueState q;
  q.enqueue("bok_1", "/a.epub", 100);
  const uint32_t before = q.generation();
  q.finish("bok_1", true, "");
  EXPECT_NE(q.generation(), before);
}

TEST(QueueStatePulse, GenerationAdvancesOnCancelAllThatEmptiesSomething) {
  QueueState q;
  q.enqueue("bok_1", "/a.epub", 100);
  const uint32_t before = q.generation();
  q.cancelAll();
  EXPECT_NE(q.generation(), before);
}

TEST(QueueStatePulse, GenerationIgnoresProgressTicks) {
  QueueState q;
  q.enqueue("bok_1", "/a.epub", 1000);
  q.markDownloading("bok_1");
  const uint32_t before = q.generation();
  q.updateProgress("bok_1", 100, 1000);
  q.updateProgress("bok_1", 900, 1000);
  // The row shows no percentage, so a per-chunk repaint would be pure cost.
  EXPECT_EQ(q.generation(), before);
}

TEST(QueueStatePulse, GenerationIgnoresARejectedEnqueue) {
  QueueState q;
  q.enqueue("bok_1", "/a.epub", 100);
  uint32_t before = q.generation();
  EXPECT_EQ(q.enqueue("bok_1", "/a.epub", 100), QueueState::EnqueueResult::AlreadyQueued);
  EXPECT_EQ(q.generation(), before);

  for (size_t i = 1; i < MAX_QUEUE; i++) {
    ASSERT_EQ(q.enqueue("bok_fill_" + std::to_string(i), "/x.epub", 1), QueueState::EnqueueResult::Ok);
  }
  before = q.generation();
  EXPECT_EQ(q.enqueue("bok_overflow", "/x.epub", 1), QueueState::EnqueueResult::Full);
  EXPECT_EQ(q.generation(), before);
}

TEST(QueueStatePulse, GenerationIgnoresNoOpTransitions) {
  QueueState q;
  q.enqueue("bok_1", "/a.epub", 100);
  q.enqueue("bok_2", "/b.epub", 200);

  uint32_t before = q.generation();
  q.markDownloading("bok_2");  // not the front
  EXPECT_EQ(q.generation(), before);

  before = q.generation();
  q.finish("bok_2", true, "");  // not the front
  EXPECT_EQ(q.generation(), before);

  QueueState empty;
  before = empty.generation();
  empty.cancelAll();  // nothing to empty
  EXPECT_EQ(empty.generation(), before);
}

TEST(QueueStatePulse, GenerationIgnoresARepeatedMarkDownloading) {
  QueueState q;
  q.enqueue("bok_1", "/a.epub", 100);
  q.markDownloading("bok_1");
  const uint32_t before = q.generation();
  q.markDownloading("bok_1");  // already Downloading -- nothing changed
  EXPECT_EQ(q.generation(), before);
}

TEST(QueueStatePulse, CompletionsCountOnlySuccessfulFinishes) {
  QueueState q;
  q.enqueue("bok_1", "/a.epub", 100);
  q.enqueue("bok_2", "/b.epub", 200);
  q.enqueue("bok_3", "/c.epub", 300);

  EXPECT_EQ(q.completions(), 0u);
  q.finish("bok_1", true, "");
  EXPECT_EQ(q.completions(), 1u);

  q.finish("bok_2", false, "fetch_failed");  // a failure is not a completion
  EXPECT_EQ(q.completions(), 1u);

  q.finish("bok_3", true, "");
  EXPECT_EQ(q.completions(), 2u);
}

TEST(QueueStatePulse, CompletionsIgnoreANoOpFinishAndCancelAll) {
  QueueState q;
  q.enqueue("bok_1", "/a.epub", 100);
  q.enqueue("bok_2", "/b.epub", 200);

  q.finish("bok_2", true, "");  // not the front -- no item concluded
  EXPECT_EQ(q.completions(), 0u);

  q.cancelAll();  // a cancel is not a completion
  EXPECT_EQ(q.completions(), 0u);

  q.finish("bok_1", true, "");  // already dropped by the cancel
  EXPECT_EQ(q.completions(), 0u);
}

}  // namespace
