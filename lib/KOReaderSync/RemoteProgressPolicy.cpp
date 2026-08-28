#include "RemoteProgressPolicy.h"

namespace remote_progress_policy {

namespace {

bool isThisDevice(const std::string& remoteDeviceId, const std::string& selfDeviceId) {
  if (remoteDeviceId.empty()) return false;
  if (remoteDeviceId == LEGACY_DEVICE_ID) return true;
  return !selfDeviceId.empty() && remoteDeviceId == selfDeviceId;
}

}  // namespace

Decision decide(const Input& in) {
  if (!in.haveRemote) return Decision::Ignore;
  if (isThisDevice(in.remoteDeviceId, in.selfDeviceId)) return Decision::Ignore;

  const float delta = in.remotePercentage - in.localPercentage;
  const float magnitude = delta < 0.0f ? -delta : delta;
  if (magnitude < PROMPT_THRESHOLD) return Decision::Ignore;

  return Decision::Prompt;
}

}  // namespace remote_progress_policy
