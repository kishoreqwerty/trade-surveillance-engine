add_library(tse_db STATIC
    db_config.cpp
    alert_store.cpp
)
target_include_directories(tse_db PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tse_db PUBLIC tse_pipeline tse_detectors tse_fix libpqxx::pqxx)

# libpq.a alone is missing symbols (pg_strcasecmp, scram_exchange, etc.)
# that only live in libpgcommon.a/libpgport.a -- see the libpq section of
# the root CMakeLists.txt for why these two archives exist outside the
# normal PostgreSQL::PostgreSQL imported target and have to be linked here
# explicitly.
target_link_libraries(tse_db PUBLIC
    "${TSE_LIBPQ_INSTALL_DIR}/lib/libpgcommon.a"
    "${TSE_LIBPQ_INSTALL_DIR}/lib/libpgport.a"
)

# Read at runtime by AlertStore::apply_schema() -- mirrors the
# QUICKFIX_FIX42_SPEC_PATH (cpp/tests/fix/) / ML_SERVICE_DIR
# (cpp/tests/ml_client/) convention already used elsewhere in this project
# for "a real file this module needs, found via an absolute path injected at
# configure time" rather than guessing a path relative to the working
# directory a test binary happens to be run from.
target_compile_definitions(tse_db PRIVATE TSE_DB_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/schema.sql")
