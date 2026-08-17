#pragma once

// Pure decision for "should an automatic library sync run right now?" --
// factored out of HomeActivity.cpp/main.cpp so the rule can be host-tested
// without ESP-IDF/Arduino (see test/sync_trigger_policy). The device-only
// callers (HomeActivity::render(), SyncSettingsActivity's manual "Sync Now")
// live where they can actually read WiFi.status()/SYNC_STORE/activityManager
// and are not tested here -- see those files' comments for how each input
// below is obtained.
//
// The constraints this encodes, from the task brief:
//  - Never before the device is paired -- nothing to sync otherwise.
//  - Never by bringing WiFi up itself: only fires when WiFi is already
//    connected. Auto-connecting on every visit to the library screen would
//    wake radio + battery on a device meant to sit idle for days.
//  - At most once per boot: `alreadyAttemptedThisBoot` is the caller's own
//    one-shot latch (a plain static, reset only by a reboot -- which this
//    device also goes through on every sleep wake, so "once per boot" and
//    "once per wake" are the same event here).
//  - Never while a book is open is enforced upstream, by only ever calling
//    this from the library screen (HomeActivity) in the first place -- not
//    re-checked here, since a pure function has no activity to ask.
namespace sync_trigger {

bool shouldAutoSync(bool paired, bool wifiConnected, bool alreadyAttemptedThisBoot);

// Pure decision for "should the deferred book-finished event be delivered
// right now?" -- same shape as shouldAutoSync() and the same reasoning for
// never bringing WiFi up itself, but gated per *library-screen visit*
// (`alreadyAttemptedThisVisit`), not per boot: HomeActivity is recreated
// every time Home is (re-)entered (see shouldAutoSync's own comment on why
// that matters for a static latch), and a fresh visit is exactly the right
// retry granularity here -- unlike a full manifest fetch, a single small
// telemetry POST is cheap enough to retry on every distinct visit rather
// than being throttled to once per boot, and doing so lets a second book
// finished later in the same boot (without an intervening sleep/reboot)
// still get attempted once the user leaves and comes back to Home, instead
// of waiting for the next reboot. Still at most once *within* a visit,
// which matters: without that, a stuck failure (e.g. the server is down)
// would retry on every single render pass while the user sits on Home --
// each one a blocking network call on the render task.
bool shouldDeliverPendingBookFinished(bool hasPending, bool paired, bool wifiConnected, bool alreadyAttemptedThisVisit);

// Pure decision for "should enterDeepSleep() run the headless before-sleep
// KOSync progress upload?" (src/sync/SleepProgressSync.h). Unlike the two
// functions above, this one deliberately brings WiFi up itself if the other
// two conditions hold -- there is no "come back on a later library visit"
// for a device about to lose power, so paying the WiFi-search cost here
// (bounded, see SleepProgressSync.h's timeout) is the only chance this
// session's progress gets sent at all.
//  - paired: the device has a working KOSync credential to send to --
//    SyncCredentialStore::isPaired() AND KOReaderCredentialStore's
//    hasEffectiveCredentials() (a pairing can succeed without provisioning
//    progress-sync if the server doesn't support it). A manually-configured,
//    never-paired KOSync account is deliberately excluded: the task this
//    encodes is "pair once and never think about it again", not "sync every
//    KOSync user automatically" -- see this task's report.
//  - isReaderActivity: activityManager.isReaderActivity() at the top of
//    enterDeepSleep(), before goToSleep() tears the reader down. With no
//    book open there is no position to build a payload from.
//  - dirty: EpubReaderActivity::hasUnsyncedProgress() -- the reader's
//    position has moved since this book was opened. Without this, opening a
//    book and immediately powering off (nothing new to report) would still
//    pay the WiFi+TLS cost for no reason.
bool shouldSyncBeforeSleep(bool paired, bool isReaderActivity, bool dirty);

}  // namespace sync_trigger
