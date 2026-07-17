add_library(tse_ingestion STATIC
    event_codec.cpp
    kafka_producer.cpp
    kafka_consumer.cpp
)
target_include_directories(tse_ingestion PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tse_ingestion PUBLIC tse_fix rdkafka++ Threads::Threads)
