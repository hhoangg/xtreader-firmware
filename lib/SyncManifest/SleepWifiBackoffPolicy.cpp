#include "SleepWifiBackoffPolicy.h"

namespace sleep_wifi_backoff {

bool shouldAttempt(const State& state) { return state.skipsRemaining == 0; }

State afterSkippedAttempt(const State& state) {
  State next = state;
  if (next.skipsRemaining > 0) next.skipsRemaining--;
  return next;
}

bool reachedNetwork(const bool wifiConnected, const bool transportFailed) { return wifiConnected && !transportFailed; }

State afterAttempt(const State& state, const bool reached) {
  if (reached) return State{};  // reset entirely once the network was actually reached

  State next = state;
  if (next.consecutiveFailures < MAX_SKIP) next.consecutiveFailures++;
  next.skipsRemaining = next.consecutiveFailures;
  return next;
}

}  // namespace sleep_wifi_backoff
