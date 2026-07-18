add_library(tse_api STATIC
    live_book_registry.cpp
    json_encode.cpp
    api_server.cpp
)
target_include_directories(tse_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tse_api PUBLIC tse_db tse_pipeline tse_orderbook Crow::Crow)

# The live demo server binary -- Phase 9's actual "Done when" entrypoint:
# a real, long-running process a dashboard can be pointed at and see
# correctly-displayed alerts as they're generated against a real book.
# Not part of any test target; started manually (see README.md) or by the
# verification steps that produced this phase's report.
add_executable(tse_api_server main.cpp)
target_link_libraries(tse_api_server PRIVATE tse_api tse_simulator tse_fix tse_ml_client)
# Same QUICKFIX_FIX42_SPEC cache variable the root CMakeLists.txt sets from
# QuickFIX's own FetchContent'd source tree -- mirrors
# QUICKFIX_FIX42_SPEC_PATH's test-only equivalent in cpp/tests/fix/.
target_compile_definitions(tse_api_server PRIVATE TSE_API_QUICKFIX_FIX42_SPEC="${QUICKFIX_FIX42_SPEC}")
