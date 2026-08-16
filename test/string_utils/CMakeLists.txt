add_executable(StringUtilsTest
  StringUtilsTest.cpp
  ${REPO_ROOT}/src/util/StringUtils.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(StringUtilsTest PRIVATE
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(StringUtilsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(StringUtilsTest)
