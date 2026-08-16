#include "BookServerDelete.h"

#include <Logging.h>

#include "SyncCredentialStore.h"
#include "network/HttpDownloader.h"

namespace book_server_delete {

Result deleteFromServer(const std::string& id) {
  Result result;
  if (!SYNC_STORE.isPaired()) {
    result.error = "not_paired";
    return result;
  }

  const std::string url = SYNC_STORE.getBaseUrl() + "/library/" + id;
  std::string response;
  const bool ok = HttpDownloader::deleteResource(url, response, &result.httpStatus, SYNC_STORE.getAccessToken());

  // outStatus is left at its initial -1 only on a connect/TLS/DNS failure
  // that never got an HTTP response at all (see deleteResource's doc
  // comment) -- check that first, same precedence as Telemetry.cpp's post().
  if (result.httpStatus < 0) {
    result.error = "transport";
  } else if (result.httpStatus == 404) {
    // Already gone server-side (deleted from another device, or a stale
    // manifest id) -- the caller's desired end state already holds.
    result.ok = true;
  } else if (result.httpStatus != 200) {
    result.error = "http_status";
  } else if (!ok) {
    result.error = "transport";  // got a 200 but the body read didn't complete
  } else {
    result.ok = true;
  }
  if (!result.ok) {
    LOG_ERR("SYNCDEL", "DELETE /library/%s failed (ok=%d status=%d)", id.c_str(), ok, result.httpStatus);
  }
  return result;
}

}  // namespace book_server_delete
