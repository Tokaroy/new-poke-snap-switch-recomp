// SPDX-License-Identifier: MIT
// Photo capture filter under render scale (analysis/core/photo_capture_quality.md). The game's Lanczos3 resamplers
// (CaptureLanczos3PS: scene -> 1080x720 photo, Lanczos3PS) size their kernel from textureSize(). The support buffer's
// render_scale is 1.0, so on a scaled source they filter on the HOST grid: at scale 2 the 1080x720 photo is a 3x decimation
// with a 1-output-pixel kernel (aliased, jaggy grass) instead of the original 1.5x. The variant filters on the guest grid.
#pragma once
#include <cstddef>
namespace nvn {
struct Host; struct ProgramImpl; struct TextureImpl;
// The program with its fragment stage replaced by a guest-grid Lanczos3 (each guest texel = box average of its host footprint),
// or nullptr when `p` is not one of the game's Lanczos3 resamplers, the source is not scaled relative to the target, or
// NVN_NO_CAPTURE_FIX=1.
ProgramImpl* CaptureLanczosVariant(Host* h, ProgramImpl* p, const TextureImpl* src, const TextureImpl* rt0, size_t nTex);
// 0, or which capture resampler `p`'s fragment stage is: 1 Lanczos3PS, 2 CaptureLanczos3PS, 3 CapturePS (cached per program).
int CaptureProgKind(ProgramImpl* p);
}  // namespace nvn
