add_executable(XtreaderIconTest
  XtreaderIconTest.cpp
)

target_include_directories(XtreaderIconTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(XtreaderIconTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(XtreaderIconTest)
