#include <cstdint>
#include <iostream>
#include <string>

#include "serialization/csv_writer.hpp"
#include "serialization/fix_writer.hpp"
#include "serialization/json_writer.hpp"
#include "simulator.hpp"

namespace {

void print_usage() {
    std::cerr << "usage: simulator_gen --severity <0.0-1.0> [--format json|csv|fix] [--seed N]\n";
}

}  // namespace

int main(int argc, char** argv) {
    double severity = 0.5;
    std::string format = "json";
    uint64_t seed = 42;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--severity" && i + 1 < argc) {
            severity = std::stod(argv[++i]);
        } else if (arg == "--format" && i + 1 < argc) {
            format = argv[++i];
        } else if (arg == "--seed" && i + 1 < argc) {
            seed = static_cast<uint64_t>(std::stoull(argv[++i]));
        } else {
            print_usage();
            return 1;
        }
    }

    tse::simulator::SimulatorConfig config;
    config.random_seed = seed;
    config.session_duration_ns = 300LL * 1'000'000'000;  // 5 minutes — small enough to eyeball
    config.baseline_orders_per_second = 2.0;
    config.wash_trade = {2, severity};
    config.spoofing_layering = {2, severity};
    config.marking_the_close = {2, severity};
    config.front_running = {2, severity};

    auto output = tse::simulator::generate_simulation(config);

    if (format == "json") {
        std::cout << tse::simulator::to_labeled_json(output.orders, output.executions) << "\n";
    } else if (format == "csv") {
        std::cout << tse::simulator::orders_to_csv(output.orders);
        std::cout << tse::simulator::executions_to_csv(output.executions);
    } else if (format == "fix") {
        for (const auto& message : tse::simulator::to_fix_messages(output.orders, output.executions)) {
            std::cout << message << "\n";
        }
    } else {
        print_usage();
        return 1;
    }

    return 0;
}
