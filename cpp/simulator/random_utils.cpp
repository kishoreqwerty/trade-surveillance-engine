#include "random_utils.hpp"

#include <algorithm>
#include <cmath>

namespace tse::simulator {

double uniform_double(std::mt19937_64& rng, double lo, double hi) {
    std::uniform_real_distribution<double> dist(lo, hi);
    return dist(rng);
}

int64_t uniform_int64(std::mt19937_64& rng, int64_t lo, int64_t hi) {
    if (hi <= lo) return lo;
    std::uniform_int_distribution<int64_t> dist(lo, hi);
    return dist(rng);
}

int64_t poisson_interarrival_ns(std::mt19937_64& rng, double rate_per_sec) {
    std::exponential_distribution<double> dist(rate_per_sec);
    double gap_sec = dist(rng);
    return static_cast<int64_t>(std::llround(gap_sec * 1e9));
}

}  // namespace tse::simulator
