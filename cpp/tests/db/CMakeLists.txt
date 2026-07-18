add_executable(db_tests
    alert_store_test.cpp
    alert_store_query_test.cpp
    db_alert_sink_integration_test.cpp
)
target_link_libraries(db_tests PRIVATE tse_db GTest::gtest_main)
gtest_discover_tests(db_tests PROPERTIES ENVIRONMENT "${TSE_TEST_ENVIRONMENT}")
