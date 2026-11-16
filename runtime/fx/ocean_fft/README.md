# Standalone FFT ocean

This module generates periodic deep-water displacement, geometric normals, and persistent foam from a Phillips spectrum. Two or three disjoint frequency bands use different tile sizes. It has a Vulkan compute implementation and an independent C++17 CPU reference. New code uses GPL-3.0 with the section 7 additional permission in this directory's LICENSE.

The module has no connection to the game runtime. A renderer supplies an initialized Vulkan device, compiled shader words, and a begun command buffer. No asset or shader data is needed for the CPU tests.

## Parameters and units

Distances and output displacement are feet; wind is mph; time is seconds. Convert output by 0.3048 if the receiving renderer uses meters. Coordinates are X/Z horizontal, Y up. Wind direction is measured from +X toward +Z. Outputs are texture arrays with one layer per cascade.

| Parameter | Default | Meaning |
|---|---:|---|
| `resolution` | 128 | Power of two: 64, 128, 256, or 512, per cascade |
| `cascades` | 3 | Two or three |
| `tile_feet` | 256, 1024, 4096 | Increasing repeat lengths; all active lengths must be positive |
| `wind_mph`, `wind_degrees` | 22, 0 | Wind speed and horizontal direction |
| `fetch_feet` | 100000 | Caps the Phillips large-wave scale at 0.2 times fetch; an approximation, not a calibrated JONSWAP fetch model |
| `amplitude` | 0.0005 | Dimensionless spectral strength; does not specify a significant wave height |
| `choppiness` | 1 | Horizontal displacement multiplier; positive values compress crests |
| `damping_feet` | 0.1 | Suppresses wavelengths below a small spatial scale |
| `seed` | 1 | Fixed 32-bit seed; CPU repeats are deterministic |
| `foam_threshold`, `foam_gain` | 0.8, 1 | Foam from horizontal Jacobian compression |
| `foam_decay` | 0.8 | Exponential decay rate per second |

The spectrum uses `g = 32.174 ft/s²`, `L = min(wind_fps²/g, 0.2*fetch)`, and `P(k) = amplitude * exp(-1/(kL)²) * dot(k_hat, wind_hat)² * exp(-k²*damping²) / k⁴`. Gravity-wave dispersion is `omega = sqrt(g*|k|)`. Independent complex Gaussian initial amplitudes evolve as `H(k,t) = h0(k)*exp(i*omega*t) + conj(h0(-k))*exp(-i*omega*t)`, which ensures Hermitian symmetry.

The DC bin and each Nyquist row/column are zero. This avoids ambiguous odd derivatives at self-conjugate bins. Discrete coefficients include `N² * sqrt(P * delta_k² / 2)`, matching the normalized inverse transform. Energy tests compare the realized evolved spectrum with spatial height variance using Parseval; they do not assert that a single random sample equals its ensemble expectation.

Cascade cutoffs are contiguous: cascade `c > 0` ends at `pi*N/(2*tile[c])`; cascade `c` starts at the next cascade's endpoint, or zero for the longest tile. This avoids double-counting wave energy. Finite tiles still repeat; cascades reduce visible repetition rather than eliminate it.

Supported numeric ranges keep the single-precision shader intermediates within
a practical envelope: tiles 16–65,536 feet; wind 0–200 mph; fetch 1–10,000,000
feet; amplitude 0–0.1; choppiness 0–8; damping 0–128 feet; foam gain/decay
0–100. CPU/GPU time is limited to 10,000,000 elapsed seconds in magnitude
(GPU elapsed time is nonnegative), with delta time 0–3,600 seconds. These
limits reject previously accepted finite inputs that overflow shader arithmetic.
Long-session visual phase accuracy still needs GPU testing. Previous CPU foam
must be finite and in [0,1]. Gaussian inputs use 23 random bits plus a half step,
which is exactly representable in binary32 and stays strictly between zero and one.

## GPU API and ownership

Compile the four `.comp` files to SPIR-V with the supplied `build.bat`, or:

```bat
glslangValidator -V --target-env vulkan1.1 -Ishaders shaders/spectrum_init.comp -o test-output/spectrum_init.spv
glslangValidator -V --target-env vulkan1.1 -Ishaders shaders/evolve.comp -o test-output/evolve.spv
glslangValidator -V --target-env vulkan1.1 -Ishaders shaders/ifft.comp -o test-output/ifft.spv
glslangValidator -V --target-env vulkan1.1 -Ishaders shaders/output.comp -o test-output/output.spv
```

`test-output` must exist for these individual commands. Load each binary into an aligned `uint32_t` array and fill `CreateInfo::{init,evolve,ifft,output}` with its word pointer and word count. `create` consumes those words immediately; they need not survive creation. Link `ocean_gpu.cpp` against Vulkan; it includes only the C++ standard library and Vulkan headers. The CPU reference is a separate translation unit and needs no Vulkan headers.

The device must support six compute storage-image descriptors, RG32F/RGBA32F storage and linear sampling, and at least 24 array layers for three cascades. Enable `shaderStorageImageExtendedFormats` when creating the device, then set the corresponding `CreateInfo` flag. `create` checks resource limits and format support and reports allocation/pipeline failures without returning a partial object. Each image has one direct Vulkan device-memory allocation; no allocator package is required.

```cpp
ocean_fft::CreateInfo info;
info.device = device;
info.physical = physical_device;
info.storage_image_extended_formats_enabled = true;
// Set info.params and the four SPIR-V spans here.
std::string error;
auto* ocean = ocean_fft::create(info, &error);
ocean_fft::Outputs maps;
if (ocean) {
    // commands is begun, outside any rendering scope, on the renderer's graphics+compute queue.
    bool recorded = ocean_fft::update(ocean, commands, delta_seconds, &maps, &error);
    // Submit commands in update order; sample maps only after recorded writes are ordered.
    // After all commands using ocean have completed:
    ocean_fft::destroy(ocean);
}
```

The comment marks where submission and GPU completion belong; do not destroy the object immediately after recording in actual renderer code. `destroy` does not submit, wait, or synchronize the caller's queue. CPU calls must be serialized. Record and submit updates in order on one queue family that supports both graphics and compute; cross-queue ownership/semaphore transfers are integration work.

The first update records image initialization and spectrum creation. Later updates record evolution, `2*log2(N)` inverse-FFT stages, and output conversion. The GPU FFT uses radix-2 decimation in frequency, ping-pong images, and a final bit reversal; the CPU uses an independent decimation-in-time FFT. Eight complex fields provide height, horizontal displacement, height slopes, and horizontal-displacement derivatives.

Descriptors remain unchanged after creation. Images remain in GENERAL; use that layout in the renderer's sampled-image descriptors, with repeat addressing. Start/end barriers order previous rendering reads, compute writes, and subsequent graphics/compute sampling. A caller that changes layouts must restore GENERAL before updating. A caller that abandons the first recorded command buffer must recreate the object, since initialization/time advancement records CPU state immediately. Outputs may remain in flight across ordered updates; destruction and changing devices still require actual GPU completion.

Every recorded update must be submitted exactly once in recording order. If an
update command buffer is discarded, recreate the object after completing prior
GPU work; otherwise CPU time and persistent foam history can diverge.

## Sampling and masking

For cascade `c`, sample at `uv = world_xz / tile_feet[c]`, array layer `c`, with a linear repeat sampler. `displacement.xyz` is `(Dx, height, Dz)` in feet. Sum displacement across active cascades and add it to a sufficiently tessellated water mesh. Avoid moving dry land or actors with this displacement.

`normal.xyz` is the unit normal of each cascade's displaced surface, computed from its two displacement tangents. `foam_jacobian.r` is foam in [0,1]; `.g` is `J = (1 + lambda*Dxx)*(1 + lambda*Dzz) - (lambda*Dxz)²`. Foam is `max(clamp((threshold-J)*gain,0,1), old_foam*exp(-decay*dt))`. It is a compression signal, not a physical bubble simulation. Negative J indicates folds; reduce choppiness if folded geometry is undesirable.

For exact combined-cascade normals, sum displaced positions and derive the two tangents using centered finite differences of the combined displacement, or use derivatives of the displaced world position in the fragment shader when mesh density is sufficient. Simply summing or blending per-cascade normals is an approximation at high choppiness. Foam may combine as `1 - product(1 - foam_c)`, clamped to [0,1].

Use an existing-water mask in [0,1]: `position = base_position + mask * summed_displacement`. Fade the mask smoothly toward shores, scene boundaries, and surfaces where the effect is unsuitable. Compute normals from that masked displacement so the mask gradient affects the surface correctly. Blend the resulting normal, roughness, and foam into the existing water material using the same mask; keep original reflection, depth, and transparency inputs until their integration is verified.

## Estimated cost

These are analytic estimates for three cascades, not measured GPU timings. All maps are full precision. Image storage is `184*C*N²` bytes, excluding alignment/driver overhead. FFT stages logically read two and write one RG32F value per field, or `24*8*C*N²*2*log2(N)` bytes per update before cache effects. The time range divides that traffic by an assumed sustained 100-400 GB/s; dispatch latency, trigonometry, initialization, and rendering are additional costs, while cache reuse may reduce external traffic.

| N | Images, three cascades | FFT logical traffic/update | Traffic-only estimate | Dispatches/update |
|---:|---:|---:|---:|---:|
| 64 | 2.16 MiB | 27 MiB | 0.07-0.28 ms | 14 |
| 128 | 8.63 MiB | 126 MiB | 0.33-1.32 ms | 16 |
| 256 | 34.5 MiB | 576 MiB | 1.51-6.04 ms | 18 |
| 512 | 138 MiB | 2592 MiB | 6.79-27.18 ms | 20 |

The first update adds one spectrum-init dispatch and three image clears. Two cascades use two-thirds of the listed image storage and logical traffic. Profile 64/128 first; GPU timing, cache behavior, and target renderer budget remain integration checks. Double-precision CPU reference values and single-precision shader values are numerically comparable, not bit-identical. Long elapsed times lose GPU phase precision; long-session tolerance is unmeasured.

## Tests and integration status

Run `build.bat` from this directory. It compiles the CPU test with `cl` or `g++`, runs it, then compiles every compute shader if `glslangValidator` is available. Return 0 means CPU checks passed and any available shader compilation passed; return 2 means no CPU compiler was available. Shader-tool absence is printed as a skip.

CPU tests cover independent naive-DFT comparison, Hermitian symmetry, real-valued fields, realized-spectrum energy, repeated/different seeds, normalized finite outputs, calm water, foam decay, and invalid parameters. The mailbox environment had no native compiler or shader validator on PATH; native execution and shader compilation remain unverified. No renderer integration or game run has occurred.

Before integration, compile all code/shaders, run the CPU suite, compare GPU readbacks against the CPU at selected times/seeds, run Vulkan synchronization validation over multiple updates and teardown, and measure the target GPU. Check masked shores, combined normals, folds, and long-session phase precision in the receiving renderer.
