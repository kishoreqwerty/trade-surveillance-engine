add_executable(orderbook_tests
    order_book_test.cpp
    depth_snapshot_test.cpp
)
target_link_libraries(orderbook_tests PRIVATE tse_orderbook tse_fix GTest::gtest_main)
gtest_discover_tests(orderbook_tests PROPERTIES ENVIRONMENT "${TSE_TEST_ENVIRONMENT}")
