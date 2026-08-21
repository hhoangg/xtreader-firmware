#pragma once

#include <cstdint>

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
//  - shouldAutoSync() itself never brings WiFi up: it only fires when WiFi
//    is already connected. Bringing WiFi up first, bounded and only when
//    paired, is shouldAttemptLibraryWifiConnect()'s job below -- see that
//    function for why a bounded bring-up is now worth the cost.
//  - At most once per boot: `alreadyAttemptedThisBoot` is the caller's own
//    one-shot latch (a plain static, reset only by a reboot -- which this
//    device also goes through on every sleep wake, so "once per boot" and
//    "once per wake" are the same event here).
//  - Never while a book is open is enforced upstream, by only ever calling
//    this from the library screen (HomeActivity) in the first place -- not
//    re-checked here, since a pure function has no activity to ask.
namespace sync_trigger {

// Deadlines for the crosspoint-sync network calls the *should I sync*
// decisions above gate: WiFi.status() == WL_CONNECTED only proves the
// device associated with an access point, not that the internet behind it
// actually works -- a hotel/cafe captive portal, exactly where a traveller
// ends up, answers every TCP connect (so it can serve its login page) while
// nothing real gets through. The only reliable way to tell the two apart is
// a real attempt against our own server with a short deadline; duplicating
// that as a separate probe would cost the same as just bounding the real
// request, so every crosspoint-sync call site passes one of these two
// instead of adding a probe (see this task's report).
//
// AUTO_SYNC_TIMEOUT_MS bounds an attempt nothing is waiting on: the
// automatic manifest sync + its piggybacked heartbeat (HomeActivity), the
// deferred book-finished delivery (BookFinishedNotifier), and the sleep
// heartbeat (SleepProgressSync) -- all invisible on failure, so short is
// right even at the cost of occasionally giving up on a slow-but-real
// connection. Order-of-magnitude matched to SleepProgressSync.h's own
// WIFI_CONNECT_TIMEOUT_MS budget for the same reasoning: long enough for one
// real TLS handshake to a reachable server (well under 1s in the common
// case), short enough that a captive portal or black-holed server does not
// stall the render task, or a power-off behind it, for long.
//
// EXPLICIT_SYNC_TIMEOUT_MS bounds something the reader asked for directly
// (Sync Now, request-books, the pre-sleep KOSync upload once WiFi is up):
// the same ~15s KOReaderSyncClient's own default already gives an explicit
// KOSync action, named here once instead of repeating the same magic number
// with the same intent at every crosspoint-sync call site.
constexpr uint32_t AUTO_SYNC_TIMEOUT_MS = 3000;
constexpr uint32_t EXPLICIT_SYNC_TIMEOUT_MS = 15000;

// The library screen's automatic manifest sync sits between the two above and
// gets its own budget rather than sharing AUTO_SYNC_TIMEOUT_MS. Two reasons,
// both measured rather than assumed:
//
//  - It is not on the power-off path. AUTO_SYNC_TIMEOUT_MS is short because a
//    dead network must not stall a power-off (SleepProgressSync) or a
//    background telemetry POST nobody is waiting on. This one runs at Home
//    behind a visible "Syncing library" popup with the owner watching, so a
//    few extra seconds cost patience, not the feeling of a device that will
//    not turn off.
//  - 3000 ms was empirically too tight for it while being ample for the
//    heartbeat that follows on the same connection: on hardware the manifest
//    GET ran out waiting for the response's first line while the heartbeat
//    POST right after it completed comfortably. A manifest page is a much
//    larger server-side operation than a heartbeat, so the two do not belong
//    on one number.
//
// Note this bound is per page, not per sync: SyncManifest.h's pager can fetch
// several, so a large library pays it more than once. SyncManifest.cpp logs
// each page's actual wall time against its budget, which is the number to
// look at before changing this.
constexpr uint32_t HOME_SYNC_TIMEOUT_MS = 8000;

bool shouldAutoSync(bool paired, bool wifiConnected, bool alreadyAttemptedThisBoot);

// Pure decision for "should the library screen bring WiFi up itself, right
// now, so shouldAutoSync() above has a chance to fire?" -- HomeActivity
// calls this FIRST, and only if it says yes brings WiFi up itself (bounded,
// back-off shared with the sleep path -- see
// src/sync/SleepProgressSync.h's connectToSavedWifi()/loadWifiBackoffState()/
// saveWifiBackoffState() and lib/SyncManifest/SleepWifiBackoffPolicy.h)
// before re-checking shouldAutoSync() with whatever WiFi.status() is
// afterward.
//
// This supersedes shouldAutoSync()'s original "never bring WiFi up itself"
// rule: on a device that reboots on every sleep wake, that rule meant the
// automatic sync never ran in production at all (nothing else ever
// connects WiFi -- see this task's report). The radio-up cost is now
// bounded (WIFI_CONNECT_TIMEOUT_MS, decaying to nothing via the shared
// back-off once repeated attempts find nothing), which is smaller than the
// feature not working.
//  - paired: identical to shouldAutoSync's own check -- nothing to sync for
//    an unpaired device, so no reason to ever wake the radio.
//  - wifiConnected: if WiFi is already up (whatever brought it up), there is
//    nothing to gain from a bring-up attempt.
//  - alreadyAttemptedThisBoot: once per boot, same granularity and static
//    latch pattern as shouldAutoSync's own -- a failed/backed-off Wi-Fi
//    search is a blocking, watchdog-relevant cost on the render task, and
//    the fact "is there Wi-Fi in range" does not change between one Home
//    visit and the next within the same boot, so retrying it on every visit
//    (e.g. bouncing in and out of File Browser) would pay that cost
//    repeatedly for no new information -- see this task's report.
bool shouldAttemptLibraryWifiConnect(bool paired, bool wifiConnected, bool alreadyAttemptedThisBoot);

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

// How many boots must pass between two lock-screen wallpaper syncs
// (src/sync/WallpaperSync.h). Boots, not hours: this board has no RTC and
// sleep is a full power cut, so a boot count persisted to SD
// (CrossPointState::bootsSinceWallpaperSync) is the only cadence unit that
// survives a wake at all.
//
// Wallpapers are the opposite of books in how often they change: books
// arrive whenever their owner uploads one and the reader wants them on the
// next visit, so the manifest sync runs every boot; a wallpaper set is
// arranged once and then left alone for weeks.
//
// This cadence is the BACKSTOP, not how a newly attached wallpaper normally
// arrives. Delivery is the heartbeat's job: every boot that gets WiFi up
// already POSTs /devices/heartbeat, and the response carries an opaque
// fingerprint of this device's assigned set, so a change made in the web UI
// is picked up on the very next boot for zero extra requests and zero extra
// TLS handshakes (see shouldSyncWallpapers()'s revision inputs below). The
// count here only has to cover what that path cannot: a heartbeat that
// failed, a server too old to send the field at all, and a fingerprint that
// happens to collide with the stored one. 8 boots keeps that safety net
// cheap -- one extra TLS handshake per 8 wakes rather than one per wake.
//
// A sync that had to stop early -- more assigned wallpapers were missing
// than one sync will download (see lib/WallpaperSync/WallpaperReconcile.h's
// MAX_DOWNLOADS_PER_SYNC) -- deliberately leaves the counter *at* this value
// instead of resetting it to zero, so the next boot picks the work back up
// rather than waiting out a whole cadence with a half-filled directory.
constexpr uint16_t WALLPAPER_SYNC_BOOT_INTERVAL = 8;

// Pure decision for "should the wallpaper sync run right now?" -- the same
// shape as shouldAutoSync() above, with two independent reasons to fire:
// the boot cadence has run out, or the heartbeat says the assigned set is
// not the one this device last synced against.
//  - paired / wifiConnected / alreadyAttemptedThisBoot: identical in meaning
//    to shouldAutoSync()'s, including that this never brings WiFi up itself.
//    It rides on whatever the library sync's own bring-up already
//    established, which is why HomeActivity runs it after that one.
//  - bootsSinceLastSync: CrossPointState::bootsSinceWallpaperSync, the count
//    of boots that reached the library screen since the last successful
//    wallpaper sync. Defaults to UINT16_MAX on a device that has never
//    synced (or whose state.json predates the field), so a freshly paired
//    reader gets its wallpapers on the first boot rather than in eight.
//  - heartbeatWallpaperRevision: the fingerprint this boot's heartbeat came
//    back with (telemetry::TelemetryResult::wallpaperRevision), or 0 when
//    there isn't one -- no heartbeat ran this boot, it failed, or the server
//    predates the field.
//  - lastSyncedWallpaperRevision:
//    CrossPointState::lastSyncedWallpaperRevision, the fingerprint the last
//    successful wallpaper sync ran against, 0 when none is stored.
//
// Both revisions are opaque: compared for equality, never for order. A
// non-zero heartbeat revision that differs from the stored one fires a sync
// immediately, which is what makes "Add to device" in the web UI land on the
// next boot instead of within WALLPAPER_SYNC_BOOT_INTERVAL. 0 on either side
// means "unknown" and is inert in both directions: it can never trigger a
// sync on its own, and it never suppresses the cadence.
bool shouldSyncWallpapers(bool paired, bool wifiConnected, bool alreadyAttemptedThisBoot, uint16_t bootsSinceLastSync,
                          uint32_t heartbeatWallpaperRevision, uint32_t lastSyncedWallpaperRevision);

}  // namespace sync_trigger
