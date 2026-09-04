add_executable(SyncedPositionMarkerTest
  SyncedPositionMarkerTest.cpp
)

target_include_directories(SyncedPositionMarkerTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
)

target_link_libraries(SyncedPositionMarkerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SyncedPositionMarkerTest)
