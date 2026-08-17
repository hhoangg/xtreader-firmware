add_executable(HeartbeatPayloadTest
  HeartbeatPayloadTest.cpp
  ${REPO_ROOT}/lib/Telemetry/HeartbeatPayload.cpp
)

target_include_directories(HeartbeatPayloadTest PRIVATE
  ${REPO_ROOT}/lib/Telemetry
)

target_link_libraries(HeartbeatPayloadTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HeartbeatPayloadTest)
