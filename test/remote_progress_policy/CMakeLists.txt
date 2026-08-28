add_executable(RemoteProgressPolicyTest
  RemoteProgressPolicyTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/RemoteProgressPolicy.cpp
)

target_include_directories(RemoteProgressPolicyTest PRIVATE
  ${REPO_ROOT}/lib/KOReaderSync
)

target_link_libraries(RemoteProgressPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(RemoteProgressPolicyTest)
