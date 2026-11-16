// SPDX-License-Identifier: GPL-3.0-only
// Additional linking permission: see ../LICENSE.
#include "../ocean_cpu.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

using namespace ocean_fft;
namespace {
unsigned checks = 0;
void require(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void fft_against_dft() {
    constexpr unsigned n = 8;
    std::vector<Complex> input(n * n);
    for (unsigned i = 0; i < input.size(); ++i) input[i] = {std::sin(i * 0.71), std::cos(i * 0.37)};
    auto fft = input; inverse_fft_2d(fft, n);
    double error = 0;
    for (unsigned z = 0; z < n; ++z) for (unsigned x = 0; x < n; ++x) {
        Complex dft = 0;
        for (unsigned kz = 0; kz < n; ++kz) for (unsigned kx = 0; kx < n; ++kx)
            dft += input[kz * n + kx] * std::polar(1.0, 2 * kPi * double(kx * x + kz * z) / n);
        dft /= n * n;
        error = std::max(error, std::abs(dft - fft[z * n + x]));
    }
    require(error < 1e-12, "CPU inverse FFT differs from naive DFT");
    std::printf("FFT versus independent 8x8 DFT: max error %.3g\n", error);
}
void ocean_fields() {
    Params p; p.resolution = 64;
    CpuOcean a(p), b(p);
    const auto frame = a.evaluate(7.25), repeated = b.evaluate(7.25);
    const size_t plane = size_t(p.resolution) * p.resolution;
    double symmetry = 0, energy_error = 0;
    for (unsigned c = 0; c < p.cascades; ++c) {
        const auto h = a.spectrum(c, 7.25);
        double spectral_energy = 0, spatial_energy = 0;
        for (unsigned z = 0; z < p.resolution; ++z) for (unsigned x = 0; x < p.resolution; ++x) {
            const size_t i = size_t(z) * p.resolution + x;
            const size_t mirror = size_t((p.resolution - z) % p.resolution) * p.resolution + (p.resolution - x) % p.resolution;
            symmetry = std::max(symmetry, std::abs(h[i] - std::conj(h[mirror])));
            spectral_energy += std::norm(h[i]);
            spatial_energy += frame.displacement[c * plane + i].y * frame.displacement[c * plane + i].y;
        }
        spectral_energy /= double(plane) * plane;
        spatial_energy /= plane;
        const double relative = std::abs(spectral_energy - spatial_energy) / std::max(1e-20, spectral_energy);
        energy_error = std::max(energy_error, relative);
        require(relative < 1e-10, "height energy violates Parseval for the evolved spectrum");
    }
    require(symmetry < 1e-10, "h(-k) is not conjugate(h(k))");
    require(frame.max_imaginary < 1e-9, "inverse fields are not real");
    bool finite = true, identical = true;
    for (size_t i = 0; i < frame.foam.size(); ++i) {
        const Vec3& d = frame.displacement[i]; const Vec3& other = repeated.displacement[i];
        const Vec3& n = frame.normal[i];
        finite &= std::isfinite(d.x + d.y + d.z + n.x + n.y + n.z + frame.foam[i] + frame.jacobian[i]);
        identical &= d.x == other.x && d.y == other.y && d.z == other.z && frame.foam[i] == repeated.foam[i];
        require(std::abs(n.x * n.x + n.y * n.y + n.z * n.z - 1) < 1e-10, "normal is not normalized");
        require(frame.foam[i] >= 0 && frame.foam[i] <= 1, "foam outside [0,1]");
    }
    require(finite, "fields contain nonfinite values"); require(identical, "same seed is not deterministic");
    ++p.seed;
    const auto changed = CpuOcean(p).evaluate(7.25);
    bool different = false;
    for (size_t i = 0; i < frame.foam.size(); ++i) different |= frame.displacement[i].y != changed.displacement[i].y;
    require(different, "different seed produced identical heights");
    std::printf("Ocean 64x64 x3: symmetry %.3g; max imaginary %.3g; relative Parseval error %.3g; repeat seed identical\n",
                symmetry, frame.max_imaginary, energy_error);
}
void calm_and_validation() {
    Params p; p.resolution = 64; p.wind_mph = 0;
    CpuOcean calm(p);
    auto previous = calm.evaluate(0);
    for (double& f : previous.foam) f = 0.7;
    const auto frame = calm.evaluate(1, 1, &previous);
    const double expected = 0.7 * std::exp(-double(p.foam_decay));
    for (size_t i = 0; i < frame.foam.size(); ++i) {
        require(frame.displacement[i].x == 0 && frame.displacement[i].y == 0 && frame.displacement[i].z == 0, "calm water moves");
        require(frame.jacobian[i] == 1 && frame.normal[i].y == 1, "calm water geometry differs");
        require(std::abs(frame.foam[i] - expected) < 1e-12, "foam is not time based");
    }
    require(validate(p), "valid parameters rejected");
    p.resolution = 96; require(!validate(p), "non-power-of-two resolution accepted");
    p.resolution = 64; p.cascades = 4; require(!validate(p), "four cascades accepted");
    p.cascades = 2; p.tile_feet[1] = p.tile_feet[0]; require(!validate(p), "unordered cascade sizes accepted");
    p.tile_feet[1] = 1024; p.wind_mph = std::numeric_limits<float>::quiet_NaN(); require(!validate(p), "NaN accepted");
    bool threw = false;
    try { (void)calm.evaluate(0, -1); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "negative dt accepted");
    auto rejects = [&](auto action, const char* message) {
        bool rejected = false;
        try { action(); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, message);
    };
    rejects([&] { (void)calm.spectrum(0, std::numeric_limits<double>::max()); }, "unsafe phase time accepted");
    rejects([&] { (void)calm.evaluate(0, 3601); }, "oversized dt accepted");
    previous.foam[0] = std::numeric_limits<double>::infinity();
    rejects([&] { (void)calm.evaluate(0, 0, &previous); }, "nonfinite previous foam accepted");
    std::vector<Complex> empty;
    rejects([&] { inverse_fft_2d(empty, 65536); }, "overflow/mismatched FFT dimensions accepted");
    std::vector<Complex> single{Complex(2, 3)};
    inverse_fft_2d(single, 1); require(single[0] == Complex(2, 3), "one-element FFT changed");
    p = Params{}; p.wind_mph = std::numeric_limits<float>::max();
    require(!validate(p), "overflowing GPU wind accepted");
    p = Params{}; p.tile_feet[0] = std::numeric_limits<float>::denorm_min();
    require(!validate(p), "subnormal tile accepted");
    std::printf("Calm geometry, exponential foam decay, and invalid parameters: passed\n");
}
}  // namespace
int main() {
    fft_against_dft(); ocean_fields(); calm_and_validation();
    std::printf("PASS: %u CPU checks\n", checks);
    return 0;
}
