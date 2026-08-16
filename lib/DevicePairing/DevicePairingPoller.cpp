#include "DevicePairingPoller.h"

namespace {
// Wraparound-safe "has nowMs reached deadlineMs" (millis() wraps every ~49
// days; matches the static_cast<int32_t>(a - b) idiom used elsewhere in this
// codebase, e.g. src/main.cpp's CMD:HTTPGET body-read deadline).
bool elapsed(uint32_t nowMs, uint32_t deadlineMs) { return static_cast<int32_t>(nowMs - deadlineMs) >= 0; }
}  // namespace

void DevicePairingPoller::start(uint32_t nowMs, uint32_t intervalSec, uint32_t expiresInSec) {
  const uint32_t interval = intervalSec != 0 ? intervalSec : DEFAULT_INTERVAL_SEC;
  const uint32_t expiresIn = expiresInSec != 0 ? expiresInSec : DEFAULT_EXPIRES_IN_SEC;
  intervalMs_ = interval * 1000;
  if (intervalMs_ < MIN_INTERVAL_MS) intervalMs_ = MIN_INTERVAL_MS;
  expiresAtMs_ = nowMs + expiresIn * 1000;
  nextPollAtMs_ = nowMs + intervalMs_;
  state_ = DevicePairingPollState::WAITING;
}

bool DevicePairingPoller::dueForPoll(uint32_t nowMs) {
  if (state_ != DevicePairingPollState::WAITING) return false;
  if (elapsed(nowMs, expiresAtMs_)) {
    state_ = DevicePairingPollState::EXPIRED;
    return false;
  }
  return elapsed(nowMs, nextPollAtMs_);
}

void DevicePairingPoller::onPollResult(uint32_t nowMs, DevicePairingPollOutcome outcome) {
  if (state_ != DevicePairingPollState::WAITING) return;
  switch (outcome) {
    case DevicePairingPollOutcome::AUTHORIZATION_PENDING:
    case DevicePairingPollOutcome::TRANSPORT_ERROR:
      nextPollAtMs_ = nowMs + intervalMs_;
      break;
    case DevicePairingPollOutcome::SLOW_DOWN:
      intervalMs_ += SLOW_DOWN_BACKOFF_MS;
      nextPollAtMs_ = nowMs + intervalMs_;
      break;
    case DevicePairingPollOutcome::ACCESS_DENIED:
      state_ = DevicePairingPollState::DENIED;
      break;
    case DevicePairingPollOutcome::EXPIRED_TOKEN:
      state_ = DevicePairingPollState::EXPIRED;
      break;
  }
}

uint32_t DevicePairingPoller::secondsRemaining(uint32_t nowMs) const {
  if (elapsed(nowMs, expiresAtMs_)) return 0;
  return (expiresAtMs_ - nowMs) / 1000;
}
