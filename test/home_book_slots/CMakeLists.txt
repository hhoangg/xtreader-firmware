add_executable(HomeBookSlotsTest
  HomeBookSlotsTest.cpp
  ${REPO_ROOT}/lib/HomeBookSlots/HomeBookSlots.cpp
)

target_include_directories(HomeBookSlotsTest PRIVATE
  ${REPO_ROOT}/lib/HomeBookSlots
)

target_link_libraries(HomeBookSlotsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HomeBookSlotsTest)
