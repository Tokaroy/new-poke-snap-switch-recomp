// SPDX-License-Identifier: MIT
// Free camera clean shot (docs/freecam.md): a PNG of the finished 3D scene at the game's render size, without the game's HUD or the host overlay.
//   nvnHostFreecamShot()   host/mods/freecam.cpp asks for one (F9, controller Y, route token fcshot)
//   FcShotAfterDraw        nvn_draw.cpp, right after the tonemap / AA draws (the same point nvn_framegen.cpp takes its HUD-less copy at):
//                          copies the window texture into a host buffer
//   FcShotAfterSubmit      nvn_vk.cpp present, after the frame's submit: waits for the copy and writes <screenshots>\freecam\freecam_<time>_<n>.png
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

namespace nvn {
struct Host;
struct ReplayCtx;
void FcShotAfterDraw(ReplayCtx& cx, const char* fragKey12);
void FcShotAfterSubmit(Host* h, VkSemaphore timeline, uint64_t value);
}  // namespace nvn
extern "C" void nvnHostFreecamShot(void);
