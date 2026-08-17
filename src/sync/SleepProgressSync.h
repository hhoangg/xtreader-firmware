#pragma once

// Headless "sync reading progress before the device powers off" -- the
// device-only half of enterDeepSleep()'s (main.cpp) before-sleep sync.
// lib/SyncManifest/SyncTriggerPolicy.h's shouldSyncBeforeSleep() is the pure
// decision for *whether* to capture a payload at all; this module is the
// *how* for the network half only: bring WiFi up from saved credentials
// (bounded, and backed off after repeated failures -- see
// lib/SyncManifest/SleepWifiBackoffPolicy.h), upload the payload the caller
// already captured, and log what it cost. It does NOT read the reader
// activity itself -- see Activity::captureProgressForSleep() /
// ActivityManager::captureReaderProgressForSleep(), which main.cpp calls
// *before* the reader activity is destroyed, well before this runs.
//
// Never called with anything to lose on failure: KOReaderSyncClient's own
// heap gate and TLS timeout already bound the network half (see
// KOReaderSyncClient.cpp), and a failed, skipped, or backed-off sync here
// just means this session's progress goes up on a later occasion instead --
// the position itself was already saved to disk by captureProgressForSleep()
// regardless of what happens here.
#include "KOReaderSyncClient.h"
#include "SleepWifiBackoffPolicy.h"

namespace sleep_progress_sync {

// Upper bound on the WiFi bring-up attempted here (scanning + associating
// with each saved network in turn), *after* the sleep screen has already
// painted (see main.cpp's enterDeepSleep()) -- the device already looks off
// by the time this spends any of this budget. Cut hard from the 8s this used
// to be: that number was chosen back when the popup+sleep-screen sequence
// still made the owner wait for it, but pointed at nothing when no saved
// network is in range, it stalled the panel-looks-off-to-actually-off gap by
// the full 8s on every single power-off away from home. 2.5s is long enough
// for one real AP association + DHCP lease (typically well under 1.5s on
// this radio) and short enough that repeated failures do not compound into a
// noticeable stall; see SleepWifiBackoffPolicy.h for what happens on repeat
// failure, and SleepProgressSync.cpp's powerButtonPressedAgain() for the
// escape hatch if even this is too long for a given moment. The KOSync
// upload itself (KOReaderSyncClient::updateProgress()) adds its own deadline
// on top of this once WiFi associates -- SyncTriggerPolicy.h's
// AUTO_SYNC_TIMEOUT_MS, not KOReaderSyncClient's ~15s explicit-action
// default: association alone does not prove the internet behind it works (a
// captive portal answers it too), so this stays short instead of trusting
// WiFi.status() the way the first version of this fix did.
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 2500;

// Brings WiFi up (bounded by WIFI_CONNECT_TIMEOUT_MS, subject to
// SleepWifiBackoffPolicy.h's back-off after repeated failures) and, if that
// succeeds, uploads the already-captured progress payload. Does NOT tear
// WiFi back down -- enterDeepSleep() already does that unconditionally a few
// lines later regardless of whether this ran.
//
// Callers must gate on sync_trigger::shouldSyncBeforeSleep() and a
// successful ActivityManager::captureReaderProgressForSleep() first: this
// function does not re-check pairing or dirtiness itself.
//
// Returns true only once progress was actually confirmed sent. Logs free
// heap and largest allocatable block before/after (as "[TEST]" JSON, gated
// on CP_TEST_CONSOLE like every other on-device diagnostic in this
// codebase) so the real worst-case memory cost is visible when built with
// `pio run -e test`. Never logs a token or a sync key.
bool trySyncBeforeSleep(const KOReaderProgress& progress);

// Bounded Wi-Fi bring-up shared with trySyncBeforeSleep() above: tries the
// last-connected saved network first (WifiCredentialStore), then every
// other saved credential in turn, each sliced by its own per-network
// timeout, all bounded overall by WIFI_CONNECT_TIMEOUT_MS. A no-op
// returning true immediately if WiFi is already connected. Does not tear
// WiFi down on failure, and does not touch the back-off state below --
// callers gate on shouldAttempt()/update via afterAttempt() around this
// call, same as trySyncBeforeSleep() does internally.
//
// Exposed (not file-local) so HomeActivity's once-per-boot library-screen
// Wi-Fi bring-up (see SyncTriggerPolicy.h's shouldAttemptLibraryWifiConnect())
// can drive the exact same connect logic instead of a second copy of it --
// this runs on the render task there, same watchdog-safety reasoning as the
// sleep path (see tryCredential()'s resetTaskWatchdogIfSubscribed() call in
// the .cpp).
//
// `cancelled` is set if the power button was pressed again during the
// search -- the owner's escape hatch, same as trySyncBeforeSleep()'s.
//
// `callerHoldsRenderLock` must be true when the calling task already holds
// ActivityManager's rendering mutex for the duration of this call (e.g.
// HomeActivity::render()'s RenderLock&& parameter -- see
// ActivityManager::renderTaskLoop(), which holds it across the whole
// render() call) and false otherwise (e.g. enterDeepSleep(), running on the
// main/loop task with no lock held). renderingMutex is a plain FreeRTOS
// mutex, not a recursive one: this function needs it only to guard
// WifiCredentialStore's SD access (shared SPI bus with the display), and
// re-taking it from a task that already holds it would deadlock that task
// against itself forever. When true, this trusts the caller and skips
// taking its own lock; when false, it takes one around the SD access, same
// as before this parameter existed.
bool connectToSavedWifi(bool& cancelled, bool callerHoldsRenderLock);

// Loads/saves the back-off state shared by trySyncBeforeSleep() and
// HomeActivity's library-screen Wi-Fi bring-up: both are "is there Wi-Fi
// here" attempts against the same saved credentials, so they share one
// counter (CrossPointState::sleepWifiConsecutiveFailures/
// sleepWifiSkipsRemaining) rather than each paying the back-off cost
// separately -- see SleepWifiBackoffPolicy.h. saveWifiBackoffState() is a
// no-op (no SD write) when the state did not actually change.
sleep_wifi_backoff::State loadWifiBackoffState();
void saveWifiBackoffState(const sleep_wifi_backoff::State& state);

#ifdef CP_TEST_CONSOLE
// CMD:SLEEPSYNCBENCH -- runs the exact same trySyncBeforeSleep() above, but
// forces the Wi-Fi search to a deliberately unreachable network instead of
// WifiCredentialStore's real saved list, so "no Wi-Fi in range" can be timed
// on hardware without the owner disabling their actual router. Exercises
// the real back-off state machine too. See main.cpp's
// testConsoleSleepSyncBench() for the JSON this and trySyncBeforeSleep()'s
// own [TEST] stage lines report.
bool benchTrySyncAgainstBogusNetwork(const KOReaderProgress& progress);

// CMD:CAPTIVEPORTALBENCH -- runs the exact same trySyncBeforeSleep() above
// against real, working Wi-Fi (association succeeds normally), but diverts
// the KOSync upload's target to a black-holed test address instead of the
// real server -- standing in for a captive portal or a server that accepts
// the connection and never answers, the case WIFI_CONNECT_TIMEOUT_MS/
// CMD:SLEEPSYNCBENCH above do not exercise (those cover "no Wi-Fi in
// range", not "Wi-Fi is fine, the internet behind it is not"). See
// KOReaderSyncClient::setTestBlackHoleOverride() and main.cpp's
// testConsoleCaptivePortalBench() for the JSON this and
// trySyncBeforeSleep()'s own [TEST] stage lines report.
bool benchTrySyncAgainstBlackHole(const KOReaderProgress& progress);
#endif

}  // namespace sleep_progress_sync
