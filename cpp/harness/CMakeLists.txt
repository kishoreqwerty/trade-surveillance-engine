add_library(tse_harness STATIC
    ground_truth.cpp
    replay_runner.cpp
    evaluation.cpp
)
target_include_directories(tse_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# tse_pipeline/tse_detectors/tse_ingestion/tse_fix listed before
# tse_simulator deliberately: cpp/fix/types.hpp and cpp/simulator/types.hpp
# are both quoted-included as just "types.hpp" (see e.g.
# detector_event.hpp), so which one a bare #include "types.hpp" resolves to
# depends on include-directory search order, which CMake derives from
# target_link_libraries order. tse_simulator listed first here made
# detector_event.hpp resolve to simulator::Order instead of fix::Order and
# fail to compile -- same ordering constraint cpp/api/CMakeLists.txt's
# `target_link_libraries(tse_api_server PRIVATE tse_api tse_simulator
# tse_fix tse_ml_client)` already depends on (tse_api's own chain reaches
# tse_fix before the explicit tse_simulator entry there).
target_link_libraries(tse_harness PUBLIC tse_pipeline tse_detectors tse_ingestion tse_fix tse_simulator)

# Phase 10's actual "Done when" entrypoint: prints the real precision/
# recall/F1 sweep, confusion matrices, and severity-gradient curve. Not
# part of any test target -- run manually (see README.md) to produce the
# numbers a phase report cites.
add_executable(tse_harness_eval main.cpp)
target_link_libraries(tse_harness_eval PRIVATE tse_harness)
