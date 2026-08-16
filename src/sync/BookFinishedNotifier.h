#pragma once

#include <string>

/**
 * Delivers the deferred "book finished" event (POST /events/book-finished,
 * Telemetry.h) -- the signal that lets the owner top the library up before
 * it runs dry (crosspoint-sync docs/API.md's "Feedback and telemetry").
 *
 * Split into two halves on purpose, across two different call sites:
 *  - Detecting the finish (ReaderActivity::render(), the first transition to
 *    isAtEndOfBook()) only records the fact into
 *    CrossPointState::pendingBookFinishedPath and saves it -- no network.
 *  - This module -- called from HomeActivity, gated by
 *    SyncTriggerPolicy.h's shouldDeliverPendingBookFinished() -- does the
 *    actual send, blocking. It must never be called from the reader: a TLS
 *    session (~9 KB, docs/API.md) is comfortable against the ~137 KB free at
 *    the library screen and is *not* comfortable against the ~50 KB a
 *    reading session leaves, with the book, its cache, and the framebuffer
 *    all still resident at the end-of-book screen.
 */
namespace book_finished_notifier {

// Looks up `bookPath`'s manifest id (SyncManifest.h's findIdByPath()) and,
// if found, sends the event via telemetry::bookFinished(). Blocking -- call
// only from a context with the library screen's heap headroom, same as
// sync_manifest::sync() itself.
//
// Returns true when the caller should clear the pending flag: either the
// event was delivered, or there is nothing to deliver (no manifest id known
// for this book -- a local-only book that was never synced, which will
// never resolve no matter how many times this is retried; see this task's
// report for why there is no documentHash fallback). Returns false only on
// a genuine send failure (WiFi/transport/HTTP), so the caller can leave the
// pending flag set and retry on a later library-screen visit.
bool tryDeliver(const std::string& bookPath);

}  // namespace book_finished_notifier
