add_library(tse_pipeline STATIC
    live_pipeline.cpp
    live_consumer.cpp
)
target_include_directories(tse_pipeline PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tse_pipeline PUBLIC tse_detectors tse_orderbook tse_ingestion tse_fix)
