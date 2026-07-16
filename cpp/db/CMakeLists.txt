add_library(tse_db STATIC
    db.cpp
)
target_include_directories(tse_db PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
