cmake_minimum_required(VERSION 3.20)
project(trade_surveillance_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- Sanitizers -------------------------------------------------------------
# ASan and TSan cannot be linked into the same binary (the compiler rejects
# -fsanitize=address,thread together), so the "debug config with ASan + TSan"
# is implemented as: ENABLE_SANITIZERS=ON selects an instrumentation mode via
# SANITIZER (address|thread|undefined), built as separate binaries per run.
# Phase 3/6 call for both an ASan pass and a TSan pass over the same code —
# not one binary carrying both simultaneously.
option(ENABLE_SANITIZERS "Enable sanitizer instrumentation (debug config)" OFF)
set(SANITIZER "address" CACHE STRING "Sanitizer used when ENABLE_SANITIZERS=ON: address, thread, or undefined")
set_property(CACHE SANITIZER PROPERTY STRINGS address thread undefined)

if(ENABLE_SANITIZERS)
  if(SANITIZER STREQUAL "address")
    set(_tse_sanitize_flags -fsanitize=address,undefined -fno-omit-frame-pointer)
  elseif(SANITIZER STREQUAL "thread")
    set(_tse_sanitize_flags -fsanitize=thread -fno-omit-frame-pointer)
  elseif(SANITIZER STREQUAL "undefined")
    set(_tse_sanitize_flags -fsanitize=undefined -fno-omit-frame-pointer)
  else()
    message(FATAL_ERROR "Unknown SANITIZER '${SANITIZER}' — expected address, thread, or undefined")
  endif()
  add_compile_options(${_tse_sanitize_flags} -g -O0)
  add_link_options(${_tse_sanitize_flags})
endif()

# Environment CTest applies to every discovered test (see each
# cpp/tests/*/CMakeLists.txt's `gtest_discover_tests(... PROPERTIES
# ENVIRONMENT "${TSE_TEST_ENVIRONMENT}")`). Empty except under
# SANITIZER=thread, where it points TSan at tsan_suppressions.txt — see that
# file and cpp/fix/README.md for exactly which races it suppresses and why
# (confirmed QuickFIX-internal, not this project's code). This is what lets
# `ctest` show a genuinely clean signal for TSan runs: without it, those
# three known third-party races would either spam every future phase's TSan
# output or (worse) cause the process to abort at exit — see
# cpp/fix/README.md's "SIGABRT at exit" section — and make CI treat a
# fully-passing run as a failure.
set(TSE_TEST_ENVIRONMENT "")
if(ENABLE_SANITIZERS AND SANITIZER STREQUAL "thread")
  set(TSE_TEST_ENVIRONMENT
      "TSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tsan_suppressions.txt")
endif()

add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)

# --- GoogleTest ---------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

# --- QuickFIX -------------------------------------------------------------
# Phase 2's FIX session layer (cpp/fix/) uses the real QuickFIX C++ engine,
# not a hand-rolled parser — see HLD §5 for why FIX/QuickFIX was chosen.
# QuickFIX's generated FIX42 message classes are checked into its own repo
# (no XML codegen step at build time), so this is a plain FetchContent build
# like googletest, just a heavier one (~40 source files). Its own
# examples/tests/SSL/DB backends are all disabled — we only need the core
# engine (session state machine, socket transport, FIX42 message classes).
set(HAVE_SSL OFF CACHE BOOL "" FORCE)
set(HAVE_MYSQL OFF CACHE BOOL "" FORCE)
set(HAVE_POSTGRESQL OFF CACHE BOOL "" FORCE)
set(HAVE_ODBC OFF CACHE BOOL "" FORCE)
set(HAVE_PYTHON3 OFF CACHE BOOL "" FORCE)
set(QUICKFIX_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(QUICKFIX_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QUICKFIX_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  quickfix
  GIT_REPOSITORY https://github.com/quickfix/quickfix.git
  GIT_TAG v1.16.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(quickfix)

# Path to the FIX 4.2 data dictionary shipped in QuickFIX's own source tree —
# used by cpp/fix's session settings and tests instead of vendoring a copy.
set(QUICKFIX_FIX42_SPEC "${quickfix_SOURCE_DIR}/spec/FIX42.xml" CACHE INTERNAL "")

# --- librdkafka -------------------------------------------------------------
# Phase 3's durable/replayable layer behind the SPSC ring buffer (cpp/ingestion/)
# — see HLD §5: SPSC gives the lock-free hot path, Kafka gives the durable log
# Phase 10's evaluation harness replays from. Uses the real librdkafka C++
# wrapper (RdKafka::Producer/KafkaConsumer, src-cpp/rdkafkacpp.h), not a
# hand-rolled protocol client. All optional backends (SSL, SASL, CURL, ZSTD)
# auto-detect and gracefully disable if not present on this machine — none
# are needed for talking to the unauthenticated local Kafka in
# docker-compose.yml. Static, matching this project's other FetchContent'd
# dependencies; its own examples/tests are disabled.
set(RDKAFKA_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(RDKAFKA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RDKAFKA_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  librdkafka
  GIT_REPOSITORY https://github.com/confluentinc/librdkafka.git
  GIT_TAG v2.15.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(librdkafka)

# --- cpp-httplib --------------------------------------------------------
# Phase 7's blocking-with-timeout HTTP client (cpp/ml_client/) talks to the
# Python ML microservice over plain local HTTP — cpp-httplib is a small,
# widely-used single-header client/server library, adequate for a call
# this project deliberately keeps off the synchronous hot path (see
# CLAUDE.md) and out of any latency-critical code. HTTPS/compression
# support is disabled: nothing here ever needs it (ml_service/ is a local,
# unauthenticated dev service, matching this project's Kafka/TimescaleDB
# posture), and disabling it avoids pulling in OpenSSL/zlib/brotli as
# transitive dependencies for a feature never used.
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_ZLIB OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_BROTLI OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.18.3
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(httplib)

# --- Modules (cpp/) -------------------------------------------------------
add_subdirectory(cpp/simulator)
add_subdirectory(cpp/fix)
add_subdirectory(cpp/ingestion)
add_subdirectory(cpp/orderbook)
add_subdirectory(cpp/detectors)
add_subdirectory(cpp/pipeline)
add_subdirectory(cpp/ml_client)
add_subdirectory(cpp/db)
add_subdirectory(cpp/api)
add_subdirectory(cpp/harness)

add_subdirectory(cpp/tests)
