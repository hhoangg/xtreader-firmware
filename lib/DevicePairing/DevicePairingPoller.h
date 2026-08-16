#pragma once

#include <cstdint>

// RFC 8628 device-flow poll state machine, decoupled from HTTP/JSON so it is
// testable with fake millis() values (no device, no network). The caller:
//   1. start() once the deviceCode/userCode pair is issued (POST /device/code).
//   2. Every loop tick, call dueForPoll(now). When it returns true, send the
//      actual POST /device/token and feed the outcome back via
//      onPollResult() -- this class never touches the network itself.
//   3. Check state(): WAITING means keep going; DENIED/EXPIRED are terminal
//      and the caller should stop polling (a 200 response is handled
//      entirely by the caller -- this machine only tracks the "still
//      waiting" side, so there is no SUCCEEDED state here).
//
// This device has no RTC (see crosspoint-sync docs/API.md): every deadline
// here is a millis()-delta, never wall-clock, and exists for display/
// give-up purposes only -- it is not a substitute for the server's own
// expired_token answer, which is still authoritative.
enum class DevicePairingPollOutcome : uint8_t {
  AUTHORIZATION_PENDING,
  SLOW_DOWN,
  ACCESS_DENIED,
  EXPIRED_TOKEN,
  // No HTTP response at all (DNS/TLS/connect failure mid-poll). Treated like
  // AUTHORIZATION_PENDING -- a single flaky request should not abort a
  // five-minute pairing window; a connection that never recovers still ends
  // the flow via the local expiry give-up below.
  TRANSPORT_ERROR,
};

enum class DevicePairingPollState : uint8_t {
  IDLE,     // start() not called yet
  WAITING,  // waiting for the user to approve, or for the next poll to come due
  DENIED,   // access_denied -- terminal, do not retry
  EXPIRED,  // expired_token from the server, or the local expiresIn timer ran out -- offer a new code
};

class DevicePairingPoller {
 public:
  // Begins tracking a freshly issued device/userCode pair. intervalSec and
  // expiresInSec come straight from the POST /device/code response (0 is
  // treated as "not specified" and clamped to a sane minimum/default).
  void start(uint32_t nowMs, uint32_t intervalSec, uint32_t expiresInSec);

  DevicePairingPollState state() const { return state_; }

  // True exactly when a POST /device/token should be sent right now: the
  // poll interval has elapsed and the code has not expired. Also flips
  // state() to EXPIRED (without touching the network) once expiresInSec has
  // elapsed since start() -- "give up when expiresIn elapses" so Wi-Fi does
  // not stay on polling a dead code forever. Returns false once state() is
  // no longer WAITING.
  bool dueForPoll(uint32_t nowMs);

  // Feeds the result of the POST /device/token just sent. Updates the next
  // poll time (or backs the interval off, for SLOW_DOWN) and/or transitions
  // to a terminal state. A no-op once state() is no longer WAITING.
  void onPollResult(uint32_t nowMs, DevicePairingPollOutcome outcome);

  // Whole seconds until expiry, floored at 0 -- for the on-screen countdown.
  uint32_t secondsRemaining(uint32_t nowMs) const;

  // Current poll interval in seconds (grows on SLOW_DOWN).
  uint32_t intervalSec() const { return intervalMs_ / 1000; }

 private:
  DevicePairingPollState state_ = DevicePairingPollState::IDLE;
  uint32_t nextPollAtMs_ = 0;
  uint32_t expiresAtMs_ = 0;
  uint32_t intervalMs_ = 0;

  // Falls back to the API's documented defaults when the server omits a
  // field (0): 300s validity, 15s poll interval.
  static constexpr uint32_t DEFAULT_EXPIRES_IN_SEC = 300;
  static constexpr uint32_t DEFAULT_INTERVAL_SEC = 15;
  // RFC 8628 SS3.5 recommends backing off by a few seconds on slow_down.
  static constexpr uint32_t SLOW_DOWN_BACKOFF_MS = 5000;
  // Guards against interval=0 (would otherwise make dueForPoll() true on
  // every single loop tick).
  static constexpr uint32_t MIN_INTERVAL_MS = 1000;
};
