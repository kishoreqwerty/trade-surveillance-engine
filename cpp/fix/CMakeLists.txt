set(_tse_fix_sources
    types.cpp
    message_translator.cpp
    fix_application.cpp
)
# Compiles in the QuickFIX-internal TSan suppressions (see
# tsan_suppressions.cpp) so they apply however the resulting binary is run,
# not only under `ctest`. Only relevant — and only built — under TSan.
if(ENABLE_SANITIZERS AND SANITIZER STREQUAL "thread")
  list(APPEND _tse_fix_sources tsan_suppressions.cpp)
endif()

add_library(tse_fix STATIC ${_tse_fix_sources})
target_include_directories(tse_fix PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tse_fix PUBLIC quickfix Threads::Threads)

# QuickFIX copies its public headers into <quickfix_SOURCE_DIR>/include via a
# separate ALL-tagged custom target (QUICKFIX_HEADERS_COPY, defined in its
# own cmake/QuickfixPrebuildSetup.cmake) that isn't otherwise ordered before
# consumers under a parallel build — without this, `cmake --build -j` can
# start compiling tse_fix before the headers exist, nondeterministically.
add_dependencies(tse_fix QUICKFIX_HEADERS_COPY)
