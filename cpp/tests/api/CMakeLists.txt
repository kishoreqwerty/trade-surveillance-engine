add_executable(api_tests
    json_encode_test.cpp
    live_book_registry_test.cpp
    api_server_test.cpp
)
target_link_libraries(api_tests PRIVATE tse_api GTest::gtest_main httplib::httplib)
gtest_discover_tests(api_tests PROPERTIES ENVIRONMENT "${TSE_TEST_ENVIRONMENT}")
