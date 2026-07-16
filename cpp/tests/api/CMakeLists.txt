add_executable(api_tests api_test.cpp)
target_link_libraries(api_tests PRIVATE tse_api GTest::gtest_main)
gtest_discover_tests(api_tests)
