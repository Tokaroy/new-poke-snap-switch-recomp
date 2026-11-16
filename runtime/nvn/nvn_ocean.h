// SPDX-License-Identifier: GPL-3.0-only
// Additional linking permission: see runtime/fx/ocean_fft/LICENSE.
// The FFT ocean (runtime/fx/ocean_fft, GPU compute) as a visual-only input of the water pass (nvn_ssr.cpp, SNAP_WATER_OCEAN). Nothing here touches game state.
#pragma once
#include "nvn_internal.h"

namespace nvn {
// Advance the ocean to the present count and record its compute work into cx.cb (the caller has no render pass open). On success *normal / *foam are the
// RGBA32F / RG32F cascade array views (layout GENERAL) and true is returned. On failure (no device support, create error) both are 1x1 dummy arrays, the
// ocean disables itself for the rest of the run, and false is returned.
bool OceanMaps(ReplayCtx& cx, VkImageView* normal, VkImageView* foam);
// 1x1 three-layer dummy array views (GENERAL) for descriptor slots that are not used; cx.cb must be recording outside a render pass the first time.
void OceanDummyViews(ReplayCtx& cx, VkImageView* normal, VkImageView* foam);
}  // namespace nvn
