#pragma once

#include <cstdint>
#include <string>

namespace tse::simulator {

// Deterministic, monotonically increasing string IDs, e.g. "ORD-000001".
class IdGenerator {
public:
    explicit IdGenerator(std::string prefix, int64_t start = 1);

    std::string next();

private:
    std::string prefix_;
    int64_t counter_;
};

}  // namespace tse::simulator
