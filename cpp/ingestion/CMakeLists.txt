add_library(tse_ingestion STATIC
    ingestion.cpp
)
target_include_directories(tse_ingestion PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
