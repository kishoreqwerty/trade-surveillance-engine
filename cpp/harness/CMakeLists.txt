add_library(tse_harness STATIC
    harness.cpp
)
target_include_directories(tse_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
