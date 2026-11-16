// SPDX-License-Identifier: GPL-3.0-only
// Additional linking permission: see LICENSE.
#pragma once
#include "ocean_types.h"
#include <complex>
#include <vector>

namespace ocean_fft {
using Complex = std::complex<double>;
struct Vec3 { double x, y, z; };
struct CpuFrame {
    std::vector<Vec3> displacement, normal;
    std::vector<double> foam, jacobian;
    double max_imaginary = 0;
};

// Normalized inverse FFT, natural input/output ordering; size may be any power of two.
void inverse_fft_2d(std::vector<Complex>& values, unsigned n);

class CpuOcean {
public:
    explicit CpuOcean(const Params& params);
    const Params& params() const { return p_; }
    std::vector<Complex> spectrum(unsigned cascade, double seconds) const;
    CpuFrame evaluate(double seconds, double dt = 0, const CpuFrame* previous = nullptr) const;
private:
    Params p_;
    std::vector<Complex> h0_;
};
}  // namespace ocean_fft
