add_executable(SyncManifestParserTest
  SyncManifestParserTest.cpp
  ${REPO_ROOT}/lib/SyncManifest/LineChunker.cpp
  ${REPO_ROOT}/lib/SyncManifest/ManifestLineParser.cpp
  ${REPO_ROOT}/lib/SyncManifest/ManifestStreamParser.cpp
  ${REPO_ROOT}/lib/SyncManifest/ManifestPager.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(SyncManifestParserTest PRIVATE
  ${REPO_ROOT}/lib/SyncManifest
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(SyncManifestParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SyncManifestParserTest)
