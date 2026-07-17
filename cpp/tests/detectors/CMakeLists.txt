add_executable(detectors_tests
    account_registry_test.cpp
    wash_trade_detector_test.cpp
    spoofing_layering_detector_test.cpp
    marking_the_close_detector_test.cpp
    front_running_detector_test.cpp
    statistical_baseline_detector_test.cpp
)
target_link_libraries(detectors_tests PRIVATE tse_detectors tse_orderbook GTest::gtest_main)
gtest_discover_tests(detectors_tests DISCOVERY_TIMEOUT 60 PROPERTIES ENVIRONMENT "${TSE_TEST_ENVIRONMENT}")
