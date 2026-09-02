add_executable(RecentDiscoveryTest
  RecentDiscoveryTest.cpp
  ${REPO_ROOT}/lib/RecentDiscovery/RecentDiscovery.cpp
)

target_include_directories(RecentDiscoveryTest PRIVATE
  ${REPO_ROOT}/lib/RecentDiscovery
)

target_link_libraries(RecentDiscoveryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(RecentDiscoveryTest)
