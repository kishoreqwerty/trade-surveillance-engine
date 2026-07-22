#pragma once

#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace tse::simulator {

double uniform_double(std::mt19937_64& rng, double lo, double hi);
int64_t uniform_int64(std::mt19937_64& rng, int64_t lo, int64_t hi);

// Draws from a piecewise-linear empirical distribution given as
// (cumulative_probability, value) points, ascending in both fields, first
// point's probability == 0.0 and last point's == 1.0 (e.g. real
// percentiles p0/p10/p25/... /p99/p100 from a WRDS TAQ export -- see
// baseline_generator.cpp's kTradeSizePercentiles/kPriceMoveMagnitudePercentiles
// for how this is used). Draws u ~ Uniform(0,1), finds which bracket u
// falls in, and linearly interpolates the value within that bracket --
// captures the real distribution's shape (including fat tails) without
// fitting a parametric family. Never clamped to any particular minimum --
// the result can be exactly points.front().second (0, if that's the
// table's own minimum, as it legitimately is for price-move magnitude,
// where the empirical p50 is 0). The table itself is the only contract:
// callers whose values must never be 0 (e.g. trade quantity) enforce that
// by never putting 0 in their own table, not by relying on this function
// to floor it for them. points must have at least 2 entries.
int64_t sample_from_percentiles(std::mt19937_64& rng, const std::vector<std::pair<double, int64_t>>& points);

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
