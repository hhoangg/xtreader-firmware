add_executable(BookCacheKeyTest
  BookCacheKeyTest.cpp
  ${REPO_ROOT}/lib/FsHelpers/PartialContentHash.cpp
  ${REPO_ROOT}/lib/FsHelpers/BookCacheMigration.cpp
)

target_include_directories(BookCacheKeyTest PRIVATE
  ${REPO_ROOT}/lib/FsHelpers
)

target_link_libraries(BookCacheKeyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookCacheKeyTest)
