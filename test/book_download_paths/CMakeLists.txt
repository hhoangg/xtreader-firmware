add_executable(BookDownloadPathsTest
  BookDownloadPathsTest.cpp
  ${REPO_ROOT}/lib/BookDownload/DownloadPaths.cpp
)

target_include_directories(BookDownloadPathsTest PRIVATE
  ${REPO_ROOT}/lib/BookDownload
)

target_link_libraries(BookDownloadPathsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookDownloadPathsTest)
