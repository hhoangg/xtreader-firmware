add_executable(SyncManifestIndexTest
  SyncManifestIndexTest.cpp
  ${REPO_ROOT}/lib/SyncManifest/LineChunker.cpp
  ${REPO_ROOT}/lib/SyncManifest/ManifestIndexFormat.cpp
  ${REPO_ROOT}/lib/SyncManifest/ManifestIndexReader.cpp
  ${REPO_ROOT}/lib/SyncManifest/ManifestIndexQuery.cpp
  ${REPO_ROOT}/lib/SyncManifest/ManifestIndexMerge.cpp
)

target_include_directories(SyncManifestIndexTest PRIVATE
  ${REPO_ROOT}/lib/SyncManifest
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(SyncManifestIndexTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SyncManifestIndexTest)
