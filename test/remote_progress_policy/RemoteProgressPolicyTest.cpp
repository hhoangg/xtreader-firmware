#include <gtest/gtest.h>

#include "RemoteProgressPolicy.h"

namespace {

using remote_progress_policy::decide;
using remote_progress_policy::Decision;
using remote_progress_policy::Input;
using remote_progress_policy::LEGACY_DEVICE_ID;

// A foreign device, far enough away to be worth asking about, unless a test
// says otherwise.
Input foreignAndFarAway() {
  Input in;
  in.haveRemote = true;
  in.remotePercentage = 0.42f;
  in.remoteDeviceId = "kindle-abc";
  in.selfDeviceId = "dev_x4";
  in.localPercentage = 0.10f;
  return in;
}

TEST(RemoteProgressPolicy, ForeignDeviceFarFromHerePrompts) {
  EXPECT_EQ(decide(foreignAndFarAway()), Decision::Prompt);
}

TEST(RemoteProgressPolicy, NothingStoredOnTheServerIsIgnored) {
  Input in = foreignAndFarAway();
  in.haveRemote = false;
  EXPECT_EQ(decide(in), Decision::Ignore);
}

TEST(RemoteProgressPolicy, OurOwnUploadIsIgnored) {
  Input in = foreignAndFarAway();
  in.remoteDeviceId = "dev_x4";
  EXPECT_EQ(decide(in), Decision::Ignore);
}

TEST(RemoteProgressPolicy, TheLegacyConstantCountsAsOurOwn) {
  // Every row this firmware wrote before per-device ids went on the wire
  // carries this value. Without this rule the device's own last upload
  // would prompt against itself on the very next open.
  Input in = foreignAndFarAway();
  in.remoteDeviceId = LEGACY_DEVICE_ID;
  EXPECT_EQ(decide(in), Decision::Ignore);
}

TEST(RemoteProgressPolicy, AnUnidentifiedRowIsNotAssumedToBeOurs) {
  // A client that never identified itself is unknown, not self -- the reader
  // still deserves the choice.
  Input in = foreignAndFarAway();
  in.remoteDeviceId = "";
  EXPECT_EQ(decide(in), Decision::Prompt);
}

TEST(RemoteProgressPolicy, SamePositionIsIgnored) {
  Input in = foreignAndFarAway();
  in.localPercentage = in.remotePercentage;
  EXPECT_EQ(decide(in), Decision::Ignore);
}

TEST(RemoteProgressPolicy, WithinTheThresholdIsIgnored) {
  // The two sides compute percentage from byte offsets independently, so
  // half a percent apart is the same place, not a different one.
  Input in = foreignAndFarAway();
  in.remotePercentage = 0.4250f;
  in.localPercentage = 0.4200f;
  EXPECT_EQ(decide(in), Decision::Ignore);
}

TEST(RemoteProgressPolicy, BeyondTheThresholdPrompts) {
  Input in = foreignAndFarAway();
  in.remotePercentage = 0.4400f;
  in.localPercentage = 0.4200f;
  EXPECT_EQ(decide(in), Decision::Prompt);
}

TEST(RemoteProgressPolicy, RemoteBehindUsAlsoPrompts) {
  // Reading on the other device does not have to move forward for the
  // question to be a real one -- it may be re-reading a chapter.
  Input in = foreignAndFarAway();
  in.remotePercentage = 0.10f;
  in.localPercentage = 0.42f;
  EXPECT_EQ(decide(in), Decision::Prompt);
}

TEST(RemoteProgressPolicy, AnUnpairedSelfIdStillTreatsTheLegacyConstantAsSelf) {
  Input in = foreignAndFarAway();
  in.selfDeviceId = "";
  in.remoteDeviceId = LEGACY_DEVICE_ID;
  EXPECT_EQ(decide(in), Decision::Ignore);
}

}  // namespace
