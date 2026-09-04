# expat is C and is not ours to keep -Wall -Wextra -pedantic clean, so it is
# built as its own target rather than through crosspoint_test_common.
add_library(chapter_xpath_expat STATIC
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)

target_include_directories(chapter_xpath_expat PUBLIC
  ${REPO_ROOT}/lib/expat
)

# Must match the device build flags in platformio.ini so the host suite
# exercises the same expat configuration the X4 runs.
target_compile_definitions(chapter_xpath_expat PUBLIC
  XML_GE=0
  XML_CONTEXT_BYTES=1024
)

add_executable(ChapterXPathResolverTest
  ChapterXPathResolverTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(ChapterXPathResolverTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/XmlParserUtils
)

target_compile_definitions(ChapterXPathResolverTest PRIVATE
  CROSSPOINT_PROGRESS_MAPPER_CPP="${REPO_ROOT}/lib/KOReaderSync/ProgressMapper.cpp"
)

target_link_libraries(ChapterXPathResolverTest PRIVATE
  chapter_xpath_expat
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ChapterXPathResolverTest)
