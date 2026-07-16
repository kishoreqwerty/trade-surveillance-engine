add_library(tse_simulator STATIC
    types.cpp
    random_utils.cpp
    id_generator.cpp
    instrument_universe.cpp
    account_registry.cpp
    baseline_generator.cpp
    abuse/wash_trade.cpp
    abuse/spoofing_layering.cpp
    abuse/marking_the_close.cpp
    abuse/front_running.cpp
    simulator.cpp
    serialization/fix_writer.cpp
    serialization/json_writer.cpp
    serialization/csv_writer.cpp
)
target_include_directories(tse_simulator PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(simulator_gen main.cpp)
target_link_libraries(simulator_gen PRIVATE tse_simulator)
