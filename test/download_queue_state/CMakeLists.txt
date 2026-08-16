add_executable(DownloadQueueStateTest
  DownloadQueueStateTest.cpp
  ${REPO_ROOT}/lib/DownloadQueue/QueueState.cpp
)

target_include_directories(DownloadQueueStateTest PRIVATE
  ${REPO_ROOT}/lib/DownloadQueue
)

target_link_libraries(DownloadQueueStateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DownloadQueueStateTest)
