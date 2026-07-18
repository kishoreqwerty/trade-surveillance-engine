add_executable(ml_client_tests
    ml_json_test.cpp
    ml_score_client_test.cpp
    ml_scoring_worker_test.cpp
    ml_anomaly_detector_test.cpp
    python_ml_service_process.cpp
    graceful_degradation_test.cpp
)
target_link_libraries(ml_client_tests PRIVATE tse_ml_client tse_pipeline tse_detectors tse_orderbook GTest::gtest_main)
# Absolute path to ml_service/ so graceful_degradation_test.cpp can launch
# the real service as a subprocess without guessing where the repo root
# is relative to the test binary's own (build-directory-dependent)
# location -- same pattern cpp/tests/fix/CMakeLists.txt already uses for
# QUICKFIX_FIX42_SPEC_PATH.
target_compile_definitions(ml_client_tests PRIVATE ML_SERVICE_DIR="${CMAKE_SOURCE_DIR}/ml_service")
gtest_discover_tests(ml_client_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 180 ENVIRONMENT "${TSE_TEST_ENVIRONMENT}")
