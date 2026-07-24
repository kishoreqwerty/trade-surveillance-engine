add_executable(harness_tests
    evaluation_test.cpp
    ground_truth_test.cpp
    json_output_test.cpp
    replay_runner_kafka_test.cpp
)
target_link_libraries(harness_tests PRIVATE tse_harness GTest::gtest_main)
gtest_discover_tests(harness_tests PROPERTIES TIMEOUT 120 ENVIRONMENT "${TSE_TEST_ENVIRONMENT}")
