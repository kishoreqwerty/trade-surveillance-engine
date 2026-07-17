add_executable(detectors_tests detectors_test.cpp)
target_link_libraries(detectors_tests PRIVATE tse_detectors GTest::gtest_main)
gtest_discover_tests(detectors_tests PROPERTIES ENVIRONMENT "${TSE_TEST_ENVIRONMENT}")
