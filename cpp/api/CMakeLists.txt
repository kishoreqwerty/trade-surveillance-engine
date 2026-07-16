add_library(tse_api STATIC
    api.cpp
)
target_include_directories(tse_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
