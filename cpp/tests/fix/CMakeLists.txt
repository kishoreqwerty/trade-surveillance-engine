add_executable(fix_tests fix_test.cpp)
target_link_libraries(fix_tests PRIVATE tse_fix GTest::gtest_main)
gtest_discover_tests(fix_tests)
