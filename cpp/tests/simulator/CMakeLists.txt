add_executable(simulator_tests
    types_and_ground_truth_test.cpp
    instrument_and_account_test.cpp
    baseline_generator_test.cpp
    abuse_wash_trade_test.cpp
    abuse_spoofing_layering_test.cpp
    abuse_marking_the_close_test.cpp
    abuse_front_running_test.cpp
    fix_writer_test.cpp
    json_csv_writer_test.cpp
    simulator_test.cpp
)
target_link_libraries(simulator_tests PRIVATE tse_simulator GTest::gtest_main)
gtest_discover_tests(simulator_tests PROPERTIES ENVIRONMENT "${TSE_TEST_ENVIRONMENT}")
