#pragma once

// Headless "sync reading progress before the device powers off" -- the
// device-only half of enterDeepSleep()'s (main.cpp) before-sleep sync.
// lib/SyncManifest/SyncTriggerPolicy.h's shouldSyncBeforeSleep() is the pure
// decision for *whether* to call this; this module is the *how*: bring WiFi
// up from saved credentials (bounded), ask the current reader activity to
// build and upload its progress, and log what it cost.
//
// Never called with anything to lose on failure: KOReaderSyncClient's own
// heap gate and TLS timeout already bound the network half (see
// KOReaderSyncClient.cpp), and a failed or skipped sync here just means this
// session's progress goes up on a later occasion instead -- see this task's
// report for why that is an acceptable trade against blocking a power-off
// indefinitely.
namespace sleep_progress_sync {

// Upper bound on the WiFi bring-up attempted here (scanning + associating
// with each saved network in turn). Chosen so a device held down to power
// off is never kept waiting past what a user holding the button would
// tolerate, even when every saved network has moved out of range: the
// KOSync upload itself (KOReaderSyncClient::updateProgress()) adds at most
// its own ~15s wolfSSL handshake deadline on top of this.
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 8000;

// Brings WiFi up (bounded by WIFI_CONNECT_TIMEOUT_MS) and, if that succeeds,
// asks the current reader activity to build and upload its progress
// (ActivityManager::syncReaderProgressForSleep()). Does NOT tear WiFi back
// down -- enterDeepSleep() already does that unconditionally a few lines
// later regardless of whether this ran.
//
// Callers must gate on sync_trigger::shouldSyncBeforeSleep() first: this
// function does not check pairing or dirtiness itself, only "is there a
// reader activity to ask" (a defensive no-op guard, not the real gate).
//
// Returns true only once progress was actually confirmed sent. Logs free
// heap and largest allocatable block before/after (as "[TEST]" JSON, gated
// on CP_TEST_CONSOLE like every other on-device diagnostic in this
// codebase) so the real worst-case memory cost is visible when built with
// `pio run -e test`. Never logs a token or a sync key.
bool trySyncBeforeSleep();

}  // namespace sleep_progress_sync
