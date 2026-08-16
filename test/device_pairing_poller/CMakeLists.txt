add_executable(DevicePairingPollerTest
  DevicePairingPollerTest.cpp
  ${REPO_ROOT}/lib/DevicePairing/DevicePairingPoller.cpp
)

target_include_directories(DevicePairingPollerTest PRIVATE
  ${REPO_ROOT}/lib/DevicePairing
)

target_link_libraries(DevicePairingPollerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DevicePairingPollerTest)
