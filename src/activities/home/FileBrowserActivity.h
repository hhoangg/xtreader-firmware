#pragma once

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
  std::vector<std::string> rowExtensions;
  // Placeholder rows only: holds the "not downloaded" marker text, rendered as the row's
  // ListItem::subtitle instead of rowExtensions' ListItem::value (see rebuildRowItems()). Empty
  // for every non-placeholder row, matching rowExtensions' "empty means unset" convention.
  std::vector<std::string> rowSubtitles;
  std::vector<freeink::ui::ListItem> rowItems;
  // getFileName()'s "[folder]" bracket formatting depends on the active
  // theme's showsFileIcons(); tracked so a theme change while this activity is
  // paused underneath (e.g. a Settings screen reached via a picker flow)
  // invalidates the cached rows on return instead of rendering stale ones.
  bool rowsUseFileIcons = false;

  void rebuildRowItems();

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
  // Whether a placeholder's book is queued or downloading, tested against a snapshot the
  // caller already took -- see rebuildRowItems() for why it is not taken per row.
  static bool isQueued(const download_queue::Snapshot& snap, const std::string& remoteId);
  size_t findEntry(const std::string& name) const;

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/",
                               Mode mode = Mode::Books);
  void onEnter() override;
  void onExit() override;
};
