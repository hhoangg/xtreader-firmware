add_executable(FileBrowserMergeTest
  FileBrowserMergeTest.cpp
  ${REPO_ROOT}/lib/FileBrowserMerge/FileBrowserMerge.cpp
)

target_include_directories(FileBrowserMergeTest PRIVATE
  ${REPO_ROOT}/lib/FileBrowserMerge
  ${REPO_ROOT}/lib/SyncManifest
)

target_link_libraries(FileBrowserMergeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FileBrowserMergeTest)
