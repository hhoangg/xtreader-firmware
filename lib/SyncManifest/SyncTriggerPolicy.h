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

}  // namespace sync_trigger
