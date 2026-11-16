// SPDX-License-Identifier: GPL-3.0-only
// Additional linking permission: see LICENSE.
#include "ocean_cpu.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ocean_fft {
namespace {
uint32_t hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x;
}
Complex gaussian(uint32_t index, uint32_t seed) {
    // 23 random bits plus a half step are exactly representable in binary32.
    const double a = ((hash(index ^ seed) >> 9) + 0.5) / 8388608.0;
    const double b = ((hash(index ^ seed ^ 0x9e3779b9u) >> 9) + 0.5) / 8388608.0;
    return std::polar(std::sqrt(-2 * std::log(a)), 2 * kPi * b);
}
int signed_bin(unsigned x, unsigned n) { return x < n / 2 ? int(x) : int(x) - int(n); }
double power(const Params& p, unsigned c, unsigned x, unsigned z) {
    if ((!x && !z) || x == p.resolution / 2 || z == p.resolution / 2) return 0;
    const double dk = 2 * kPi / p.tile_feet[c];
    const double kx = signed_bin(x, p.resolution) * dk, kz = signed_bin(z, p.resolution) * dk;
    const double k2 = kx * kx + kz * kz, k = std::sqrt(k2);
    if (k < band_low(p, c) || k >= band_high(p, c) || p.wind_mph == 0) return 0;
    const double wind = p.wind_mph * (5280.0 / 3600.0);
    const double length = std::min(wind * wind / kGravity, 0.2 * p.fetch_feet);
    const double angle = p.wind_degrees * kPi / 180;
    const double alignment = (kx * std::cos(angle) + kz * std::sin(angle)) / k;
    return p.amplitude * std::exp(-1 / (k2 * length * length)) * alignment * alignment /
           (k2 * k2) * std::exp(-k2 * p.damping_feet * p.damping_feet);
}
void inverse_1d(std::vector<Complex>& a) {
    const unsigned n = unsigned(a.size());
    for (unsigned i = 1, j = 0; i < n; ++i) {
        unsigned bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (unsigned length = 2; length <= n;) {
        const Complex step = std::polar(1.0, 2 * kPi / length);
        for (unsigned base = 0; base < n; base += length) {
            Complex w = 1;
            for (unsigned j = 0; j < length / 2; ++j) {
                const Complex u = a[base + j], v = w * a[base + j + length / 2];
                a[base + j] = u + v; a[base + j + length / 2] = u - v; w *= step;
            }
        }
        if (length == n) break; // Do not wrap the stage length back to zero.
        length <<= 1;
    }
    for (Complex& v : a) v /= n;
}
}  // namespace

void inverse_fft_2d(std::vector<Complex>& values, unsigned n) {
    if (!n || (n & (n - 1)) || size_t(n) > std::numeric_limits<size_t>::max() / n || values.size() != size_t(n) * n)
        throw std::invalid_argument("invalid FFT dimensions");
    std::vector<Complex> line(n);
    for (unsigned z = 0; z < n; ++z) {
        for (unsigned x = 0; x < n; ++x) line[x] = values[size_t(z) * n + x];
        inverse_1d(line);
        for (unsigned x = 0; x < n; ++x) values[size_t(z) * n + x] = line[x];
    }
    for (unsigned x = 0; x < n; ++x) {
        for (unsigned z = 0; z < n; ++z) line[z] = values[size_t(z) * n + x];
        inverse_1d(line);
        for (unsigned z = 0; z < n; ++z) values[size_t(z) * n + x] = line[z];
    }
}

CpuOcean::CpuOcean(const Params& params) : p_(params) {
    std::string error;
    if (!validate(p_, &error)) throw std::invalid_argument(error);
    const unsigned n = p_.resolution;
    h0_.resize(size_t(n) * n * p_.cascades);
    for (unsigned c = 0; c < p_.cascades; ++c) {
        const double dk = 2 * kPi / p_.tile_feet[c];
        for (unsigned z = 0; z < n; ++z) for (unsigned x = 0; x < n; ++x) {
            const uint32_t index = (c * n + z) * n + x;
            h0_[index] = gaussian(index, p_.seed) * (double(n) * n * std::sqrt(power(p_, c, x, z) * dk * dk / 2));
        }
    }
}

std::vector<Complex> CpuOcean::spectrum(unsigned c, double seconds) const {
    if (c >= p_.cascades || !std::isfinite(seconds) || std::abs(seconds) > kMaxSeconds) throw std::invalid_argument("invalid cascade or time");
    const unsigned n = p_.resolution;
    std::vector<Complex> out(size_t(n) * n);
    for (unsigned z = 0; z < n; ++z) for (unsigned x = 0; x < n; ++x) {
        const double dk = 2 * kPi / p_.tile_feet[c];
        const double kx = signed_bin(x, n) * dk, kz = signed_bin(z, n) * dk;
        const double omega = std::sqrt(kGravity * std::hypot(kx, kz));
        const Complex phase = std::polar(1.0, omega * seconds);
        const size_t base = size_t(c) * n * n;
        out[size_t(z) * n + x] = h0_[base + size_t(z) * n + x] * phase +
            std::conj(h0_[base + size_t((n - z) % n) * n + (n - x) % n]) * std::conj(phase);
    }
    return out;
}

CpuFrame CpuOcean::evaluate(double seconds, double dt, const CpuFrame* previous) const {
    if (!std::isfinite(dt) || dt < 0 || dt > kMaxDeltaSeconds) throw std::invalid_argument("dt must be in [0,3600] seconds");
    const unsigned n = p_.resolution;
    const size_t plane = size_t(n) * n, count = plane * p_.cascades;
    if (previous && previous->foam.size() != count) throw std::invalid_argument("previous foam dimensions differ");
    if (previous) for (double f : previous->foam)
        if (!std::isfinite(f) || f < 0 || f > 1) throw std::invalid_argument("previous foam must be finite and in [0,1]");
    CpuFrame out;
    out.displacement.resize(count); out.normal.resize(count); out.foam.resize(count); out.jacobian.resize(count);
    for (unsigned c = 0; c < p_.cascades; ++c) {
        auto height = spectrum(c, seconds);
        std::array<std::vector<Complex>, kFieldCount> fields;
        for (auto& f : fields) f.resize(plane);
        for (unsigned z = 0; z < n; ++z) for (unsigned x = 0; x < n; ++x) {
            const size_t i = size_t(z) * n + x;
            const double dk = 2 * kPi / p_.tile_feet[c];
            const double kx = signed_bin(x, n) * dk, kz = signed_bin(z, n) * dk, k = std::hypot(kx, kz);
            const Complex h = height[i], ih(-h.imag(), h.real());
            fields[Height][i] = h;
            if (k == 0) continue;
            fields[DisplaceX][i] = ih * (kx / k); fields[DisplaceZ][i] = ih * (kz / k);
            fields[SlopeX][i] = ih * kx; fields[SlopeZ][i] = ih * kz;
            fields[DerivativeXX][i] = h * (-kx * kx / k); fields[DerivativeZZ][i] = h * (-kz * kz / k);
            fields[DerivativeXZ][i] = h * (-kx * kz / k);
        }
        for (auto& f : fields) {
            inverse_fft_2d(f, n);
            for (const Complex v : f) out.max_imaginary = std::max(out.max_imaginary, std::abs(v.imag()));
        }
        for (size_t i = 0; i < plane; ++i) {
            const size_t j = c * plane + i;
            const double a = 1 + p_.choppiness * fields[DerivativeXX][i].real();
            const double b = p_.choppiness * fields[DerivativeXZ][i].real();
            const double d = 1 + p_.choppiness * fields[DerivativeZZ][i].real();
            const double hx = fields[SlopeX][i].real(), hz = fields[SlopeZ][i].real();
            const double jac = a * d - b * b;
            Vec3 normal{b * hz - d * hx, jac, b * hx - a * hz};
            const double len = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
            if (len < 1e-12) normal = {0, 1, 0};
            else { normal.x /= len; normal.y /= len; normal.z /= len; }
            out.displacement[j] = {p_.choppiness * fields[DisplaceX][i].real(), fields[Height][i].real(),
                                   p_.choppiness * fields[DisplaceZ][i].real()};
            out.normal[j] = normal; out.jacobian[j] = jac;
            const double fresh = std::clamp((p_.foam_threshold - jac) * p_.foam_gain, 0.0, 1.0);
            out.foam[j] = std::max(fresh, previous ? previous->foam[j] * std::exp(-p_.foam_decay * dt) : 0.0);
        }
    }
    return out;
}
}  // namespace ocean_fft
