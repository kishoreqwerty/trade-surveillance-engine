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

int64_t sample_from_percentiles(std::mt19937_64& rng, const std::vector<std::pair<double, int64_t>>& points) {
    // No floor imposed here: the caller's table is the sole source of
    // truth for what values are valid (deliberately not std::max'd to 1 --
    // that was this function's original behavior, correct for a trade-qty
    // table that starts at {0.0, 1}, but wrong the moment a second caller
    // needed 0 to be a real, common outcome -- see baseline_generator.cpp's
    // kPriceMoveMagnitudePercentiles, whose empirical p10/p25/p50 are all
    // 0). A table that should never produce 0 simply shouldn't have 0 as
    // its minimum point.
    const double u = uniform_double(rng, 0.0, 1.0);
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        const auto& [lo_p, lo_v] = points[i];
        const auto& [hi_p, hi_v] = points[i + 1];
        if (u <= hi_p) {
            const double bracket_width = hi_p - lo_p;
            const double t = bracket_width > 0.0 ? (u - lo_p) / bracket_width : 0.0;
            const double interpolated = static_cast<double>(lo_v) + t * static_cast<double>(hi_v - lo_v);
            return std::llround(interpolated);
        }
    }
    return points.back().second;  // u landed past the last point -- clamp
}

}  // namespace tse::simulator
