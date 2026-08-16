#pragma once

#include <string>

/**
 * DELETE /library/:id (crosspoint-sync docs/API.md) -- the "also delete on
 * the server" half of FileBrowserActivity's force-delete option. Device
 * auth, same bearer-token pattern as Telemetry.h's calls; this is its own
 * tiny module rather than a Telemetry.h addition because it is not a
 * feedback/telemetry event -- it is destructive and irreversible from the
 * device (the book leaves the owner's library for every device on the
 * account), so it gets its own explicit call site rather than blending into
 * "POST a small JSON event and move on".
 */
namespace book_server_delete {

struct Result {
  bool ok = false;
  // Empty on success. One of: "not_paired", "transport" (no HTTP response at
  // all), "http_status" (see httpStatus). A 404 ("not_found" server-side --
  // already gone) is treated as ok=true: the end state the caller wants
  // (nothing left on the server for this id) is already true either way.
  std::string error;
  int httpStatus = -1;
};

// `id` is a SyncManifest id (e.g. "bok_..."), found via
// sync_manifest::findIdByPath() for the local file being deleted.
Result deleteFromServer(const std::string& id);

}  // namespace book_server_delete
