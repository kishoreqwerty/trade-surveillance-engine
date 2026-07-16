add_library(tse_fix STATIC
    fix.cpp
)
target_include_directories(tse_fix PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
