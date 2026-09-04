#pragma once

#include <HomeBookSlots.h>

#include <memory>
#include <string>
#include <vector>

#include "ManifestIndexFormat.h"
#include "activities/UiListActivity.h"
#include "sync/DownloadQueue.h"

class FileBrowserActivity final : public UiListActivity {
 public:
  // Books = standard reader browser; PickFirmware = filter to .bin only and return path via ActivityResult.
  enum class Mode { Books, PickFirmware };

 private:
  // Deletion
  bool removeDirFile(const std::string& fullPath);
  // Local-only half of a delete: removeDirFile() + cache cleanup + refresh
  // the row list. Shared by both the plain 2-way dialog and the "Delete from
  // device" branch of the 3-way force-delete dialog.
  void performLocalDelete(const std::string& fullPath);
  // "Delete everywhere" branch: DELETE /library/:id (book_server_delete),
  // bringing WiFi up first if needed (an explicit, already-confirmed
  // destructive action, same as SyncSettingsActivity's Request Books), then
  // performLocalDelete() only if the server call actually succeeded -- a
  // failed server delete must not silently fall back to a local-only one,
  // or "delete everywhere" would sometimes quietly mean "delete here".
  void performServerDeleteThenLocal(const std::string& fullPath, const std::string& manifestId);

  Mode mode = Mode::Books;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;
  // Parallel to `files`: empty for a local entry, the manifest id for a remote-only placeholder
  // row (Mode::Books only -- see mergeRemoteEntries()). Kept separate from `files` itself (rather
  // than, say, a struct) so a plain local listing -- no index, or Mode::PickFirmware -- costs
  // nothing beyond one same-size vector of empty strings.
  std::vector<std::string> fileRemoteId;
  std::unique_ptr<char[]> fileNameBuffer;

  // Per-row render buffers, derived from `files` and rebuilt only when it
  // changes (loadFiles()) rather than on every repaint — buildScreen() used to
  // rebuild a name/extension string and a ListItem per file on every render
  // (cursor move, tap flash, ...), which meant a 500-file directory allocated
  // 500 strings per repaint instead of once per directory load.
  std::vector<std::string> rowNames;
  // The row's ListItem::value: a normal row's extension ("EPUB"), or a placeholder row's download
  // status ("Chưa tải"/"Đang tải" -- see rebuildRowItems()). One column, one meaning either way, so
  // a reader scanning the list always finds the same answer ("is this on the device?") in the same
  // place -- an empty value slot used to be the only signal a placeholder row gave, which read as
  // "no data yet" rather than "not downloaded" at a glance.
  std::vector<std::string> rowValues;
  std::vector<freeink::ui::ListItem> rowItems;
  // getFileName()'s "[folder]" bracket formatting depends on the active
  // theme's showsFileIcons(); tracked so a theme change while this activity is
  // paused underneath (e.g. a Settings screen reached via a picker flow)
  // invalidates the cached rows on return instead of rendering stale ones.
  bool rowsUseFileIcons = false;

  void rebuildRowItems();

  // Last download_queue::pulse() this activity acted on. The queue has no
  // callbacks and the row status text is derived only in rebuildRowItems(),
  // so without polling a row keeps whatever it said when the folder was
  // loaded -- "On server" after the reader picks it, "Downloading" forever
  // after it lands -- until the reader navigates out and back.
  uint32_t lastPulseGeneration = 0;
  uint32_t lastPulseCompletions = 0;

  // A popup drawn straight into the framebuffer (GUI.drawPopup) has no
  // activity of its own: nothing owns it, and the next redraw simply paints
  // over it. Every redraw on this screen used to be input-driven, which is
  // what performServerDeleteThenLocal()'s "no requestUpdate() here" comment
  // relies on to keep an error readable. pollDownloadQueue() is the first
  // redraw here that no input asked for, so it needs to stand off.
  //
  // A deadline, not a flag cleared by input: touch and swipe redraw through
  // UiListActivity::loop() without passing through anything this class
  // overrides, so a flag could stay set for the rest of the session and
  // silence the poll entirely. A deadline cannot get stuck.
  uint32_t popupHoldsScreenUntilMs = 0;
  static constexpr uint32_t POPUP_READ_MS = 3000;
  void holdScreenForPopup();
  // Refreshes the rows when the queue moved: a status-only change rebuilds
  // the row text, a completed download re-lists the folder (the book is a
  // real file now). Called from loop() on every pass; costs two integer
  // compares when nothing changed.
  void pollDownloadQueue();

  int listCount() const override { return static_cast<int>(files.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  // Long-press BACK goes to root; short Back goes up a directory (home/cancel at
  // root), and Confirm activates on RELEASE (a hold is "delete").
  bool handleCustomInput() override;
  bool handleButtons() override;
  // Header shows the current folder name (battery indicator via GUI.drawHeader);
  // footer labels depend on path depth and picker mode.
  void drawChrome() override;
  void drawFooter() override;
  // forceDelete routes the touch long-press to the delete branch; button
  // navigation leaves it false and relies on getHeldTime() instead.
  void activateSelected(bool forceDelete = false);

  // Data loading
  void loadFiles();
  // Mode::Books only: streams /.crosspoint/remote.idx (via sync_manifest::listByPrefix, scoped to
  // basepath) through file_browser_merge::FolderMerge and appends whatever it doesn't already have
  // -- remote-only books as placeholders, remote-only subfolders as normal folders -- to `files`
  // and `fileRemoteId`. Content-preserving when there is no index yet (every fileRemoteId stays
  // empty, `files` keeps the same names in the same order), matching FolderMerge's "no index ->
  // unchanged local listing" contract; only a genuine read error leaves both vectors as they were
  // before this call, rather than reassigning them to equivalent content. Called from loadFiles(),
  // after the local directory scan and before sortMergedFiles().
  void mergeRemoteEntries();
  // Re-sorts `files` directories-first/natural (same ordering as FsHelpers::sortFileList, whose
  // comparator this reuses via FsHelpers::naturalLess) while keeping `fileRemoteId` aligned to it
  // by index -- sortFileList alone would only reorder the names and desync the two vectors. Always
  // called, even when mergeRemoteEntries() added nothing, so there is exactly one sort path instead
  // of two that could drift apart.
  void sortMergedFiles();
  // Stub for the download engine's entry point (owned by another agent; see the task brief). Only
  // logs today -- TODO(download-engine): replace this body with the real request once
  // src/sync/BookDownload* exists. `destinationPath` is the full local path the book would occupy
  // once downloaded (basepath + entry name), for a future implementation to write to; `remoteId`
  // lets it re-look-up the full ManifestIndexRecord (size, contentHash, ...) via
  // sync_manifest::findById() instead of this class holding one per placeholder row.
  void requestBookDownload(const std::string& remoteId);
  // Reshapes a download_queue::Snapshot into the QueueView home_book_slots::remoteState()
  // reads -- the same reshape HomeActivity::rebuildSlots() and RecentBooksActivity::
  // buildQueueView() do. Built once per rebuildRowItems() call, not once per row: a
  // home_book_slots::QueueView owns a std::vector, so building it inside the per-row loop
  // would allocate on every placeholder row instead of once for the whole rebuild.
  static home_book_slots::QueueView toQueueView(const download_queue::Snapshot& snap);
  size_t findEntry(const std::string& name) const;

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/",
                               Mode mode = Mode::Books);
  void onEnter() override;
  void loop() override;
  void onExit() override;
};
