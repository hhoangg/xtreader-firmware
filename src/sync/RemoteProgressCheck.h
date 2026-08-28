#pragma once

#include <string>

#include "KOReaderSyncClient.h"

/**
 * One short, background "where does the server think this book is?" request,
 * started when a book opens.
 *
 * Progress sync was push-only until this existed: the device uploaded its
 * position on the way into sleep and never asked for anyone else's, so a book
 * read to chapter nine on another device opened at chapter zero here. The
 * fetch runs on its own FreeRTOS task for the same reason DownloadQueue's
 * does -- a blocking Wi-Fi bring-up plus TLS handshake on the UI task would
 * freeze page turns for several seconds, and the whole point is that the
 * reader never feels this happen.
 *
 * Every failure is silent by design: no Wi-Fi, low heap, no credentials, no
 * row on the server, a timeout, an auth error -- all of them end as "nothing
 * to say" and the reader is never told. The only visible outcome is the
 * dialog EpubReaderActivity raises when the answer is both foreign and far
 * away (lib/KOReaderSync/RemoteProgressPolicy.h decides which).
 *
 * The heap gate is the real constraint, not the task plumbing: a reading
 * session leaves ~50 KB free and KOReaderSyncClient refuses a handshake below
 * MIN_FREE_FOR_TLS. This checks the same bar before bringing the radio up, so
 * a low-memory moment costs no power either.
 *
 * At most one check is in flight at a time; a start() while one is running is
 * ignored.
 */
namespace remote_progress {

// Starts a background fetch for `documentHash` (a KOSync document id, i.e.
// what KOReaderDocumentId::calculate returns). No-op when unpaired, without
// KOSync credentials, when the hash is empty, or when a check is already
// running. Never blocks the caller.
void start(const std::string& documentHash);

// True exactly once per completed fetch, and only when the finished check was
// for `documentHash` -- a result for a book the reader has since closed is
// dropped rather than applied to whatever is open now. `outHaveProgress` is
// false when the fetch finished but there was nothing to report, which is the
// overwhelmingly common case and is not an error.
bool consume(const std::string& documentHash, KOReaderProgress& outProgress, bool& outHaveProgress);

// Forgets any pending result. Called when the reader closes, so the next book
// cannot inherit the previous one's answer.
void discard();

}  // namespace remote_progress
