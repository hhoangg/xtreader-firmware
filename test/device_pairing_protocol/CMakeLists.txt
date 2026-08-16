add_executable(DevicePairingProtocolTest
  DevicePairingProtocolTest.cpp
  ${REPO_ROOT}/lib/DevicePairing/DevicePairingProtocol.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(DevicePairingProtocolTest PRIVATE
  ${REPO_ROOT}/lib/DevicePairing
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(DevicePairingProtocolTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DevicePairingProtocolTest)
