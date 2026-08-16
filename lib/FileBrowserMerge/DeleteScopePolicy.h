#pragma once

// Pure decision for "which delete actually happens?" -- factored out of
// FileBrowserActivity.cpp so the index-to-action mapping can be host-tested
// (see test/delete_scope_policy) without ESP-IDF/Arduino or a real
// ConfirmationActivity/OptionPopup on screen.
//
// FileBrowserActivity offers a 2-way dialog (Cancel/Delete) for a plain
// local file and a 3-way dialog (Cancel/Delete from device/Delete
// everywhere) only when the file has a known crosspoint-sync manifest id
// (see SyncManifest.h's findIdByPath()) -- a local-only file has nothing to
// delete server-side, and offering it there would be a lie (task brief).
// `selectedIndex` is whatever ConfirmationActivity's OptionPopup reports:
// 0 is always Cancel by that widget's convention; 1 is the primary
// (local-only) delete; 2, only reachable when `serverDeleteOffered` is true,
// is "delete everywhere".
namespace file_delete_policy {

enum class Action { Cancelled, DeleteLocalOnly, DeleteLocalAndServer };

Action resolve(bool serverDeleteOffered, int selectedIndex);

}  // namespace file_delete_policy
