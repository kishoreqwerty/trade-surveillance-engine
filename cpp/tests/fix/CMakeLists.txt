add_executable(fix_tests
    message_translator_test.cpp
    malformed_message_test.cpp
    fix_session_test_fixture.cpp
    live_session_test.cpp
    simulator_end_to_end_test.cpp
    fix_to_pipeline_integration_test.cpp
)
target_link_libraries(fix_tests PRIVATE tse_fix tse_simulator tse_pipeline GTest::gtest_main)
target_compile_definitions(fix_tests PRIVATE QUICKFIX_FIX42_SPEC_PATH="${QUICKFIX_FIX42_SPEC}")
gtest_discover_tests(fix_tests PROPERTIES TIMEOUT 90 ENVIRONMENT "${TSE_TEST_ENVIRONMENT}")
