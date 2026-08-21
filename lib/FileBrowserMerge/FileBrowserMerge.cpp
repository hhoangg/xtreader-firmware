#include "FileBrowserMerge.h"

#include <array>
#include <cctype>
#include <utility>

namespace file_browser_merge {

namespace {

bool endsWithCaseInsensitive(const std::string_view name, const std::string_view suffix) {
  if (name.size() < suffix.size()) return false;
  const size_t offset = name.size() - suffix.size();
  for (size_t i = 0; i < suffix.size(); i++) {
    if (std::tolower(static_cast<unsigned char>(name[offset + i])) !=
        std::tolower(static_cast<unsigned char>(suffix[i]))) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool isRecognizedBookName(const std::string_view name) {
  // Mirrors FileBrowserActivity::loadFiles()'s Mode::Books filter -- see this file's header
  // comment for why it isn't just a call into FsHelpers.
  static constexpr std::array<std::string_view, 7> kExtensions{".epub", ".xtc", ".xtch", ".txt", ".md", ".bmp", ".png"};
  for (const auto& ext : kExtensions) {
    if (endsWithCaseInsensitive(name, ext)) return true;
  }
  return false;
}

FolderMerge::FolderMerge(std::string folderPrefix, std::vector<std::string> localNames)
    : prefix_(std::move(folderPrefix)) {
  entries_.reserve(localNames.size());
  for (auto& name : localNames) {
    entries_.push_back(MergedEntry{std::move(name), std::string()});
  }
}

bool FolderMerge::hasEntry(const std::string& name) const {
  for (const auto& entry : entries_) {
    if (entry.name == name) return true;
  }
  return false;
}

void FolderMerge::addRemoteRecord(const ManifestIndexRecord& record) {
  if (record.downloaded) return;  // already on the device -- see ManifestIndexRecord's comment

  if (record.path.size() <= prefix_.size() || record.path.compare(0, prefix_.size(), prefix_) != 0) {
    return;  // defensive: caller is expected to have already scoped the scan to prefix_
  }
  const std::string relative = record.path.substr(prefix_.size());
  if (relative.empty()) return;
  if (relative.front() == '.') return;  // dot-prefixed names are never listed

  const size_t slash = relative.find('/');
  if (slash != std::string::npos) {
    // The record lives in a deeper subfolder -- surface just that folder, once. Records under the
    // same subfolder are contiguous in a listByPrefix scan (sorted path order), so comparing
    // against only the last-seen folder name is enough to dedupe without rescanning entries_.
    std::string folderName = relative.substr(0, slash);
    if (folderName.empty()) return;
    if (hasLastRemoteFolder_ && lastRemoteFolder_ == folderName) return;
    lastRemoteFolder_ = folderName;
    hasLastRemoteFolder_ = true;

    std::string dirEntry = folderName + "/";
    if (!hasEntry(dirEntry)) entries_.push_back(MergedEntry{std::move(dirEntry), std::string()});
    return;
  }

  // A direct child of this folder: a placeholder, unless it's not a recognized book type or is
  // already present (local, or added by an earlier record -- ids are unique so the latter
  // shouldn't happen, but hasEntry() is cheap insurance).
  if (!isRecognizedBookName(relative)) return;
  if (hasEntry(relative)) return;
  entries_.push_back(MergedEntry{relative, record.id});
}

}  // namespace file_browser_merge
