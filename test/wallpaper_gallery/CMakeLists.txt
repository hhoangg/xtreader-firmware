add_executable(WallpaperGalleryTest
  WallpaperGalleryPathsTest.cpp
  WallpaperGalleryFeedTest.cpp
  WallpaperGridTest.cpp
  ${REPO_ROOT}/lib/WallpaperGallery/WallpaperGalleryPaths.cpp
  ${REPO_ROOT}/lib/WallpaperGallery/WallpaperGalleryFeed.cpp
  ${REPO_ROOT}/lib/WallpaperGallery/WallpaperGrid.cpp
  ${REPO_ROOT}/lib/WallpaperSync/WallpaperPaths.cpp
  ${REPO_ROOT}/lib/SyncManifest/LineChunker.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(WallpaperGalleryTest PRIVATE
  ${REPO_ROOT}/lib/WallpaperGallery
  ${REPO_ROOT}/lib/WallpaperSync
  ${REPO_ROOT}/lib/SyncManifest
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(WallpaperGalleryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WallpaperGalleryTest)
