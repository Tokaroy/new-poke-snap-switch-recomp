// SPDX-License-Identifier: GPL-3.0-only
// Additional linking permission: see this directory's LICENSE.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace ocean_fft {
constexpr double kPi = 3.14159265358979323846;
constexpr double kGravity = 32.174;  // feet / second squared
constexpr unsigned kFieldCount = 8;
constexpr double kMaxSeconds = 10000000;
constexpr double kMaxDeltaSeconds = 3600;
enum Field { Height, DisplaceX, DisplaceZ, SlopeX, SlopeZ, DerivativeXX, DerivativeZZ, DerivativeXZ };

struct Params {
    uint32_t resolution = 128;
    uint32_t cascades = 3;
    uint32_t seed = 1;
    std::array<float, 3> tile_feet = {256, 1024, 4096};
    float wind_mph = 22;
    float wind_degrees = 0;  // 0 = +X; 90 = +Z
    float fetch_feet = 100000;
    float amplitude = 0.0005f;
    float choppiness = 1;
    float damping_feet = 0.1f;
    float foam_threshold = 0.8f;
    float foam_decay = 0.8f;  // per second
    float foam_gain = 1;
};

inline bool validate(const Params& p, std::string* error = nullptr) {
    auto bad = [&](const char* s) { if (error) *error = s; return false; };
    if (p.resolution < 64 || p.resolution > 512 || (p.resolution & (p.resolution - 1)))
        return bad("resolution must be a power of two from 64 to 512");
    if (p.cascades < 2 || p.cascades > 3) return bad("two or three cascades are required");
    for (unsigned i = 0; i < p.cascades; ++i)
        if (!std::isfinite(p.tile_feet[i]) || p.tile_feet[i] < 16 || p.tile_feet[i] > 65536 || (i && p.tile_feet[i] <= p.tile_feet[i - 1]))
            return bad("cascade tile lengths must increase within 16-65536 feet");
    const float values[] = {p.wind_mph, p.wind_degrees, p.fetch_feet, p.amplitude, p.choppiness,
                            p.damping_feet, p.foam_threshold, p.foam_decay, p.foam_gain};
    for (float v : values) if (!std::isfinite(v)) return bad("parameters must be finite");
    if (p.wind_mph < 0 || p.fetch_feet <= 0 || p.amplitude < 0 || p.choppiness < 0 || p.damping_feet < 0 ||
        p.foam_decay < 0 || p.foam_gain < 0 || p.foam_threshold < 0 || p.foam_threshold > 1)
        return bad("wind/amplitude/choppiness/damping/foam rates must be nonnegative; fetch positive; foam threshold in [0,1]");
    if (p.wind_mph > 200 || p.fetch_feet < 1 || p.fetch_feet > 10000000 || p.amplitude > 0.1f ||
        p.choppiness > 8 || p.damping_feet > 128 || p.foam_decay > 100 || p.foam_gain > 100)
        return bad("parameters exceed the supported GPU numeric ranges; see README");
    return true;
}

inline double band_low(const Params& p, unsigned c) {
    return c + 1 < p.cascades ? 0.5 * kPi * p.resolution / p.tile_feet[c + 1] : 0;
}
inline double band_high(const Params& p, unsigned c) {
    return c ? 0.5 * kPi * p.resolution / p.tile_feet[c] : 1e30;
}
}  // namespace ocean_fft
