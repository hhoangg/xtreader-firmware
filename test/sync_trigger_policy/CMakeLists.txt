add_executable(SyncTriggerPolicyTest
  SyncTriggerPolicyTest.cpp
  ${REPO_ROOT}/lib/SyncManifest/SyncTriggerPolicy.cpp
)

target_include_directories(SyncTriggerPolicyTest PRIVATE
  ${REPO_ROOT}/lib/SyncManifest
)

target_link_libraries(SyncTriggerPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SyncTriggerPolicyTest)
