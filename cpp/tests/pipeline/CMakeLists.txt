add_executable(pipeline_tests
    live_pipeline_test.cpp
    live_consumer_sustained_load_test.cpp
)
target_link_libraries(pipeline_tests PRIVATE tse_pipeline tse_detectors tse_orderbook tse_simulator GTest::gtest_main)
gtest_discover_tests(pipeline_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 300 ENVIRONMENT "${TSE_TEST_ENVIRONMENT}")
