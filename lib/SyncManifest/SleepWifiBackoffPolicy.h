#pragma once

#include <cstdint>

// Pure back-off schedule for enterDeepSleep()'s headless before-sleep Wi-Fi
// attempt (src/sync/SleepProgressSync.h) -- factored out for host testing,
// same pattern as SyncTriggerPolicy.h.
//
// This device has no time source at all (no RTC, and sleep is a full chip
// reset that would lose one anyway -- see docs/README on hardware), so a
// real "wait N minutes before retrying" back-off is not available. Instead
// this counts consecutive failed *attempts*: each one adds one more sleep to
// skip before the next attempt, so a device carried away from every saved
// network pays the Wi-Fi budget once, then skips 1 sleep, then (if that
// retry also fails) skips 2, and so on up to MAX_SKIP -- never stopping
// outright, so Wi-Fi coming back into range is still found within a bounded
// number of power-offs. The two-field State is persisted across the reboot
// that every sleep already is (see CrossPointState::sleepWifiConsecutiveFailures
// / sleepWifiSkipsRemaining, saved by the same APP_STATE.saveToFile() call
// enterDeepSleep() already makes -- no extra SD write).
namespace sleep_wifi_backoff {

constexpr uint8_t MAX_SKIP = 5;

struct State {
  uint8_t consecutiveFailures = 0;
  uint8_t skipsRemaining = 0;
};

// True if this sleep should spend the Wi-Fi budget at all.
bool shouldAttempt(const State& state);

// State to persist after shouldAttempt() returned false and the attempt was
// skipped: consumes one skip, nothing else changes.
State afterSkippedAttempt(const State& state);

// True if this attempt reached the network at all: Wi-Fi associated AND the
// KOSync upload that followed did not fail at the transport level.
// `transportFailed` is the caller's own check of
// KOReaderSyncClient::updateProgress()'s result against NETWORK_ERROR
// specifically (see SleepProgressSync.cpp) -- any other outcome (AUTH_FAILED,
// SERVER_ERROR, ...) still proves a real HTTP response came back, i.e. the
// network was fine and only the account/server was not, so it must not count
// here. A captive portal or black-holed server -- associates, then the
// upload times out -- looks exactly like "no Wi-Fi here" through this
// function, which is the point: see this task's report for why WiFi
// association alone is not a reliable signal. Kept as two plain bools
// (rather than depending on KOReaderSyncClient's Error enum here) so this
// stays host-testable without pulling ESP-IDF/Arduino into this library.
bool reachedNetwork(bool wifiConnected, bool transportFailed);

// State to persist after an attempt actually ran. `reached` should be
// reachedNetwork()'s result above -- NOT whether the progress upload's HTTP
// response was itself successful. A reachable network with an HTTP-level
// failure (auth, server error) is an account/server problem, not a "no
// Wi-Fi here" problem, and must not back off.
State afterAttempt(const State& state, bool reached);

}  // namespace sleep_wifi_backoff
