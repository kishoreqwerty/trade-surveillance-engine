add_library(tse_ml_client STATIC
    ml_json.cpp
    ml_score_client.cpp
    ml_scoring_worker.cpp
    ml_anomaly_detector.cpp
)
target_include_directories(tse_ml_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tse_ml_client PUBLIC tse_pipeline tse_detectors tse_orderbook tse_ingestion tse_fix httplib::httplib)
