#pragma once

#include <cstdint>
#include <random>
#include <vector>

namespace tse::simulator {

double uniform_double(std::mt19937_64& rng, double lo, double hi);
int64_t uniform_int64(std::mt19937_64& rng, int64_t lo, int64_t hi);

// Next interarrival gap (nanoseconds) for a Poisson process at the given
// per-second event rate.
int64_t poisson_interarrival_ns(std::mt19937_64& rng, double rate_per_sec);

inline double lerp(double lo, double hi, double t) {
    return lo + (hi - lo) * t;
}

template <typename T>
const T& pick_random(const std::vector<T>& values, std::mt19937_64& rng) {
    std::uniform_int_distribution<size_t> dist(0, values.size() - 1);
    return values[dist(rng)];
}

}  // namespace tse::simulator
