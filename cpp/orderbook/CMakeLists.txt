add_library(tse_orderbook STATIC
    order_book.cpp
)
target_include_directories(tse_orderbook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tse_orderbook PUBLIC tse_fix)
