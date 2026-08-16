#include <gtest/gtest.h>

#include "DownloadPaths.h"

namespace {

TEST(TempPathFor, AppendsPartSuffix) {
  EXPECT_EQ(book_download_paths::tempPathFor("/Văn học/Nhà Giả Kim.epub"), "/Văn học/Nhà Giả Kim.epub.part");
}

TEST(TempPathFor, NeverCollidesWithTheRealExtension) {
  // A host script or file manager globbing by ".epub" must never see the
  // temp file as a real book -- the whole point of the temp-then-rename
  // pattern (ProgressFile.h's crash-safe write, applied to a whole book).
  const std::string tmp = book_download_paths::tempPathFor("/book.epub");
  EXPECT_NE(tmp.substr(tmp.size() - 5), ".epub");
}

TEST(ParentDirOf, ReturnsTheDirectoryForANestedPath) {
  EXPECT_EQ(book_download_paths::parentDirOf("/Kỹ năng/Đắc Nhân Tâm.epub"), "/Kỹ năng");
}

TEST(ParentDirOf, ReturnsSlashForAPathDirectlyUnderRoot) {
  EXPECT_EQ(book_download_paths::parentDirOf("/book.epub"), "/");
}

TEST(ParentDirOf, ReturnsEmptyForAPathWithNoDirectoryComponent) {
  EXPECT_EQ(book_download_paths::parentDirOf("book.epub"), "");
}

TEST(ParentDirOf, HandlesMultipleNestedFolders) {
  EXPECT_EQ(book_download_paths::parentDirOf("/A/B/C/book.epub"), "/A/B/C");
}

}  // namespace
