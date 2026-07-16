add_executable(harness_tests harness_test.cpp)
target_link_libraries(harness_tests PRIVATE tse_harness GTest::gtest_main)
gtest_discover_tests(harness_tests)
