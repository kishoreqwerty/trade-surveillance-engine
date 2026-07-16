add_executable(db_tests db_test.cpp)
target_link_libraries(db_tests PRIVATE tse_db GTest::gtest_main)
gtest_discover_tests(db_tests)
