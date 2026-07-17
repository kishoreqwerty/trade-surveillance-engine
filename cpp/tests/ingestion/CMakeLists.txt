add_executable(ingestion_tests
    spsc_ring_buffer_test.cpp
    event_codec_test.cpp
    two_thread_pipeline_test.cpp
    backpressure_test.cpp
    kafka_replay_test.cpp
)
target_link_libraries(ingestion_tests PRIVATE tse_ingestion tse_fix GTest::gtest_main)
gtest_discover_tests(ingestion_tests PROPERTIES TIMEOUT 120 ENVIRONMENT "${TSE_TEST_ENVIRONMENT}")
