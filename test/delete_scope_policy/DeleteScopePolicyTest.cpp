#include <gtest/gtest.h>

#include "DeleteScopePolicy.h"

namespace {

using file_delete_policy::Action;
using file_delete_policy::resolve;

TEST(DeleteScopePolicy, TwoWayDialogCancel) {
  EXPECT_EQ(resolve(/*serverDeleteOffered=*/false, /*selectedIndex=*/0), Action::Cancelled);
}

TEST(DeleteScopePolicy, TwoWayDialogConfirmIsLocalOnly) { EXPECT_EQ(resolve(false, 1), Action::DeleteLocalOnly); }

TEST(DeleteScopePolicy, ThreeWayDialogCancel) {
  EXPECT_EQ(resolve(/*serverDeleteOffered=*/true, /*selectedIndex=*/0), Action::Cancelled);
}

TEST(DeleteScopePolicy, ThreeWayDialogDeleteFromDeviceIsLocalOnly) {
  EXPECT_EQ(resolve(true, 1), Action::DeleteLocalOnly);
}

TEST(DeleteScopePolicy, ThreeWayDialogDeleteEverywhereIsLocalAndServer) {
  EXPECT_EQ(resolve(true, 2), Action::DeleteLocalAndServer);
}

TEST(DeleteScopePolicy, IndexTwoWithoutServerOfferIsNeverTreatedAsServerDelete) {
  // A local-only file's dialog never shows a 3rd option, so a selectedIndex
  // of 2 here would be a stale/impossible value -- must not be silently
  // upgraded to a server delete it never offered.
  EXPECT_EQ(resolve(/*serverDeleteOffered=*/false, /*selectedIndex=*/2), Action::Cancelled);
}

TEST(DeleteScopePolicy, NegativeIndexIsCancelled) { EXPECT_EQ(resolve(true, -1), Action::Cancelled); }

}  // namespace
