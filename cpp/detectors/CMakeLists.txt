add_library(tse_detectors STATIC
    detectors.cpp
)
target_include_directories(tse_detectors PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
