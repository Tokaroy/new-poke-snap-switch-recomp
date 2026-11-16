// SPDX-License-Identifier: MIT
// Water effects: screen-space reflections, caustics and shoreline foam (project code, ours). See nvn_ssr.cpp and shaders/water_fx.frag.
//   SNAP_WATER_SSR=1, SNAP_WATER_CAUSTICS=1, SNAP_WATER_FOAM=1 (each off by default, each its own options row).
#pragma once
#include <cstdint>

namespace nvn {
struct ReplayCtx;
// nvn_draw.cpp DoDraw, one line right after FxMidPass (before the draw is recorded): finds the game's water draws by fragment program key.
// At the first water draw it ends the render pass and copies the scene colour and depth; at the first non-water draw after the water (or at the Fx
// pre-tonemap slot when water is the last thing drawn) it runs the effect pass. true = commands were recorded: the caller re-emits viewports / scissors.
bool WaterBeforeDraw(ReplayCtx& cx, const char* fsKey, bool blend, bool depthWrite);
// true right after WaterBeforeDraw classified the current draw as water while the effects run: DoDraw forces its depth write on for the pipeline (the
// effect pass needs the surface depth; it copies the pre-water depth back afterwards, so the rest of the frame sees the game's own depth).
bool WaterForceDepthWrite();
}  // namespace nvn
