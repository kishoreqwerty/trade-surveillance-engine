add_library(tse_simulator STATIC
    simulator.cpp
)
target_include_directories(tse_simulator PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
