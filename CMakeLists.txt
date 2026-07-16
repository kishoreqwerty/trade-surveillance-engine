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

add_compile_options(-Wall -Wextra)

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

# --- Modules (cpp/) -------------------------------------------------------
add_subdirectory(cpp/simulator)
add_subdirectory(cpp/fix)
add_subdirectory(cpp/ingestion)
add_subdirectory(cpp/orderbook)
add_subdirectory(cpp/detectors)
add_subdirectory(cpp/db)
add_subdirectory(cpp/api)
add_subdirectory(cpp/harness)

add_subdirectory(cpp/tests)
