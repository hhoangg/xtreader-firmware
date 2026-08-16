#include "BookFinishedNotifier.h"

#include <Logging.h>

#include "SyncManifest.h"
#include "Telemetry.h"

namespace book_finished_notifier {

bool tryDeliver(const std::string& bookPath) {
  std::string id;
  if (!sync_manifest::findIdByPath(bookPath, id)) {
    // No documentHash fallback: unlike KOSync's document-matching hash,
    // there is no established convention for what this endpoint's
    // documentHash should contain for a book that was never synced from the
    // server, and guessing one risks the owner's dashboard silently
    // mismatching it against a real catalog entry. Nothing to deliver, ever,
    // for a book with no manifest id -- clear the pending flag rather than
    // retrying forever.
    LOG_DBG("BOOKFIN", "No manifest id known for %s -- dropping the pending book-finished event", bookPath.c_str());
    return true;
  }

  const telemetry::TelemetryResult result = telemetry::bookFinished(id);
  if (!result.ok) {
    LOG_ERR("BOOKFIN", "book-finished event failed for %s (error=%s status=%d) -- will retry on a later visit",
            bookPath.c_str(), result.error.c_str(), result.httpStatus);
    return false;
  }
  return true;
}

}  // namespace book_finished_notifier
