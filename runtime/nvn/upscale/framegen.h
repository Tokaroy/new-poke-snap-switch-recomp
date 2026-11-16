// SPDX-License-Identifier: MIT
// AMD FSR frame generation through the FidelityFX API (FidelityFX SDK 2.4.0, frame generation 4.0.2): the signed
// upscalers\amd_fidelityfx_framegeneration_vk.dll next to the exe is loaded by absolute path at run time. The builder or the
// launcher downloads it from AMD's repository after the user accepts AMD's license; it is never part of our release zip.
//
// This is the thin wrapper around the DLL (context, prepare, configure + dispatch). The GPU side (the HUD-less ring, the output
// image, the generated present) is nvn_framegen.cpp. Off unless SNAP_FRAMEGEN=1 (settings key `frame_generation`).
//
// The picture the DLL interpolates is the finished game frame (tonemap and AA, before the host UI overlay); the HUD-less image
// is a copy of the 3D scene taken right after the tonemap draw, so the game's own menus and HUD stay out of the interpolation.
// Frame generation never touches a photo: frames that run the Evaluation or a photo capture are never prepared, so they are
// presented as real frames only (nvn_framegen.cpp FgFrameAllowed).
#pragma once
#include "upscaler.h"

namespace snap_framegen {

using snap_upscale::DeviceCtx;
using snap_upscale::Image;

bool Wanted();           // SNAP_FRAMEGEN=fsr (or 1), read once
bool Load();             // the DLL beside the exe (false: unavailable, logged once)
bool Init(const DeviceCtx& c);
bool Usable();           // loaded and not failed
void Release();          // destroys the context (device idle first)

// Context for a display size, a render size and a back-buffer format (the format the DLL is TOLD; an sRGB image is described
// as UNORM and the transfer function says sRGB). Recreated when any of them change. False on failure (counted).
bool EnsureContext(uint32_t dw, uint32_t dh, uint32_t rw, uint32_t rh, VkFormat unormFormat);

// Per-frame inputs to the prepare dispatch (recorded on the scene command buffer after the motion vectors).
struct PrepareIn {
  VkCommandBuffer cb = VK_NULL_HANDLE;
  Image depth, mv;                          // depth in SHADER_READ_ONLY_OPTIMAL, mv in SHADER_READ_ONLY_OPTIMAL (render pixels)
  uint32_t w = 0, h = 0;                    // render size
  float jx = 0, jy = 0;                     // jitter (render pixels)
  float dtMs = 16.67f;
  float nearZ = 0.01f, farZ = 1000.0f, fovY = 0.6f;
  bool reset = false;
  float pos[3] = {}, right[3] = {}, up[3] = {}, fwd[3] = {};
  bool depthIsDsReadOnly = true;            // the depth image is in DEPTH_STENCIL_READ_ONLY_OPTIMAL (switched around the dispatch)
};
// Records the prepare dispatch. Returns the frame id it used (frame ids go up by exactly one per prepared frame), 0 on failure.
uint64_t Prepare(const PrepareIn& in);

// Per-present inputs (recorded on the present command buffer).
struct GenerateIn {
  VkCommandBuffer cb = VK_NULL_HANDLE;
  uint64_t frameId = 0;                     // from Prepare
  Image hudless;                            // UNORM copy of the scene before the UI (SHADER_READ_ONLY_OPTIMAL), may be empty
  Image present;                            // the finished frame (UNORM view of the sRGB image, SHADER_READ_ONLY_OPTIMAL)
  Image out;                                // output (UNORM view of the sRGB image, GENERAL)
  uint32_t w = 0, h = 0;
  bool reset = false;
};
// Configure (HUD-less colour, frame id) + dispatch one interpolated frame into `out`. False on failure.
bool Generate(const GenerateIn& in);

// The rate gate: frame generation only runs when the base rate is high enough (about 50 fps), with a 2 s hysteresis. panelHz
// <= 0 means unknown (then only the base rate counts). `current` is the present state; returns the new state.
bool RateGate(double baseHz, double panelHz, bool current, uint64_t nowNs, uint64_t* sinceNs);

uint64_t FramesGenerated();
uint32_t Failures();

}  // namespace snap_framegen
