add_executable(DeleteScopePolicyTest
  DeleteScopePolicyTest.cpp
  ${REPO_ROOT}/lib/FileBrowserMerge/DeleteScopePolicy.cpp
)

target_include_directories(DeleteScopePolicyTest PRIVATE
  ${REPO_ROOT}/lib/FileBrowserMerge
)

target_link_libraries(DeleteScopePolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DeleteScopePolicyTest)
