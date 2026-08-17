add_executable(SleepWifiBackoffPolicyTest
  SleepWifiBackoffPolicyTest.cpp
  ${REPO_ROOT}/lib/SyncManifest/SleepWifiBackoffPolicy.cpp
)

target_include_directories(SleepWifiBackoffPolicyTest PRIVATE
  ${REPO_ROOT}/lib/SyncManifest
)

target_link_libraries(SleepWifiBackoffPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SleepWifiBackoffPolicyTest)
