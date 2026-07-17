add_library(tse_detectors STATIC
    account_registry.cpp
    wash_trade_detector.cpp
    spoofing_layering_detector.cpp
    marking_the_close_detector.cpp
    front_running_detector.cpp
    statistical_baseline_detector.cpp
)
target_include_directories(tse_detectors PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tse_detectors PUBLIC tse_orderbook)
