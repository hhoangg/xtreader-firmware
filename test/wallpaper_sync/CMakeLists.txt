add_executable(WallpaperSyncTest
  WallpaperReconcileTest.cpp
  WallpaperManifestParserTest.cpp
  ${REPO_ROOT}/lib/WallpaperSync/WallpaperPaths.cpp
  ${REPO_ROOT}/lib/WallpaperSync/WallpaperReconcile.cpp
  ${REPO_ROOT}/lib/WallpaperSync/WallpaperManifest.cpp
  ${REPO_ROOT}/lib/SyncManifest/LineChunker.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(WallpaperSyncTest PRIVATE
  ${REPO_ROOT}/lib/WallpaperSync
  ${REPO_ROOT}/lib/SyncManifest
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(WallpaperSyncTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WallpaperSyncTest)
