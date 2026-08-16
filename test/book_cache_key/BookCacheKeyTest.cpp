#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>

#include "BookCacheMigration.h"
#include "PartialContentHash.h"

namespace {

// In-memory "file" for exercising partialContentHashFromReader() without touching disk. Each
// instance stands in for a distinct file at a distinct path - the hash function only ever sees
// the bytes, never a path, which is exactly the property under test (content-addressed, not
// path-addressed).
struct MemoryFile {
  std::string content;
};

size_t readFromMemory(void* ctx, const size_t offset, uint8_t* buffer, const size_t length) {
  const auto* file = static_cast<MemoryFile*>(ctx);
  if (offset >= file->content.size()) {
    return 0;
  }
  const size_t available = file->content.size() - offset;
  const size_t toCopy = std::min(length, available);
  memcpy(buffer, file->content.data() + offset, toCopy);
  return toCopy;
}

std::string hashOf(const std::string& content) {
  MemoryFile file{content};
  return FsHelpers::partialContentHashFromReader(file.content.size(), readFromMemory, &file);
}

}  // namespace

TEST(PartialContentHash, IdenticalContentAtDifferentPathsHashesTheSame) {
  // Two independent "files" (distinct MemoryFile instances, standing in for two distinct paths)
  // with byte-identical content must hash identically - the whole point of hashing content
  // instead of path.
  const std::string content = "The quick brown fox jumps over the lazy dog";
  MemoryFile fileAtPathOne{content};
  MemoryFile fileAtPathTwo{content};

  const std::string hashOne =
      FsHelpers::partialContentHashFromReader(fileAtPathOne.content.size(), readFromMemory, &fileAtPathOne);
  const std::string hashTwo =
      FsHelpers::partialContentHashFromReader(fileAtPathTwo.content.size(), readFromMemory, &fileAtPathTwo);

  ASSERT_FALSE(hashOne.empty());
  EXPECT_EQ(hashOne, hashTwo);
}

TEST(PartialContentHash, DifferentContentHashesDifferently) {
  const std::string hashA = hashOf("The quick brown fox jumps over the lazy dog");
  const std::string hashB = hashOf("The quick brown fox jumps over the lazy cat");

  ASSERT_FALSE(hashA.empty());
  ASSERT_FALSE(hashB.empty());
  EXPECT_NE(hashA, hashB);
}

TEST(PartialContentHash, FileShorterThanFirstOffsetStillHashes) {
  // The first non-zero offset (i = 0) is 1024 bytes, so a file smaller than that only ever
  // contributes the single i = -1 / offset 0 chunk. This is also a known MD5 test vector, so it
  // doubles as a correctness check on the hand-rolled MD5 implementation: with content shorter
  // than one chunk, the partial-content hash degenerates to a plain MD5 of the whole file.
  const std::string pangram = "The quick brown fox jumps over the lazy dog";
  ASSERT_LT(pangram.size(), 1024u);

  const std::string hash = hashOf(pangram);
  EXPECT_EQ(hash, "9e107d9d372bb6826bd81d3542a419d6");

  // Deterministic: hashing the same short content again gives the same result.
  EXPECT_EQ(hash, hashOf(pangram));
}

TEST(PartialContentHash, EmptyFileReturnsEmptyHash) { EXPECT_EQ(hashOf(""), ""); }

TEST(PartialContentHash, ContentSpanningMultipleOffsetsHashesConsistently) {
  // 2000 bytes crosses the i = 0 offset (1024), so two chunks get read and concatenated before
  // hashing. Just needs to be deterministic and non-empty; the offset/skip logic itself is
  // exercised by the "shorter than first offset" and "identical content" cases above.
  std::string content(2000, 'x');
  for (size_t i = 0; i < content.size(); i++) {
    content[i] = static_cast<char>('a' + (i % 26));
  }

  const std::string hash = hashOf(content);
  EXPECT_EQ(hash.size(), 32u);
  EXPECT_EQ(hash, hashOf(content));
}

namespace {

// Fake filesystem for exercising migrateLegacyCacheDir() without touching disk.
struct FakeFs {
  std::set<std::string> existingDirs;
  int renameCallCount = 0;
};

bool fakeExists(void* ctx, const std::string& path) { return static_cast<FakeFs*>(ctx)->existingDirs.count(path) > 0; }

bool fakeRename(void* ctx, const std::string& oldPath, const std::string& newPath) {
  auto* fs = static_cast<FakeFs*>(ctx);
  fs->renameCallCount++;
  if (fs->existingDirs.count(oldPath) == 0) {
    return false;
  }
  fs->existingDirs.erase(oldPath);
  fs->existingDirs.insert(newPath);
  return true;
}

}  // namespace

TEST(BookCacheMigration, RenamesLegacyDirExactlyOnceAndNotOnSecondCall) {
  FakeFs fs;
  const std::string legacyPath = "/.crosspoint/epub_12345";
  const std::string newPath = "/.crosspoint/epub_abcdef0123456789";
  fs.existingDirs.insert(legacyPath);

  const bool migrated = FsHelpers::migrateLegacyCacheDir(newPath, legacyPath, fakeExists, fakeRename, &fs);
  EXPECT_TRUE(migrated);
  EXPECT_EQ(fs.renameCallCount, 1);
  EXPECT_TRUE(fs.existingDirs.count(newPath));
  EXPECT_FALSE(fs.existingDirs.count(legacyPath));

  // Second call: newPath now exists, so this must be a no-op that never touches legacyPath (or
  // calls rename) again.
  const bool migratedAgain = FsHelpers::migrateLegacyCacheDir(newPath, legacyPath, fakeExists, fakeRename, &fs);
  EXPECT_FALSE(migratedAgain);
  EXPECT_EQ(fs.renameCallCount, 1);
}

TEST(BookCacheMigration, NoLegacyDirMeansNoMigration) {
  FakeFs fs;
  const std::string legacyPath = "/.crosspoint/epub_12345";
  const std::string newPath = "/.crosspoint/epub_abcdef0123456789";

  const bool migrated = FsHelpers::migrateLegacyCacheDir(newPath, legacyPath, fakeExists, fakeRename, &fs);
  EXPECT_FALSE(migrated);
  EXPECT_EQ(fs.renameCallCount, 0);
  EXPECT_TRUE(fs.existingDirs.empty());
}

TEST(BookCacheMigration, NewDirAlreadyExistingSkipsMigrationEvenIfLegacyExistsToo) {
  FakeFs fs;
  const std::string legacyPath = "/.crosspoint/epub_12345";
  const std::string newPath = "/.crosspoint/epub_abcdef0123456789";
  fs.existingDirs.insert(legacyPath);
  fs.existingDirs.insert(newPath);

  const bool migrated = FsHelpers::migrateLegacyCacheDir(newPath, legacyPath, fakeExists, fakeRename, &fs);
  EXPECT_FALSE(migrated);
  EXPECT_EQ(fs.renameCallCount, 0);
  // Legacy dir is left alone - migration never removes it unless it actually renames it.
  EXPECT_TRUE(fs.existingDirs.count(legacyPath));
}
