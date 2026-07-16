add_library(tse_orderbook STATIC
    orderbook.cpp
)
target_include_directories(tse_orderbook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
