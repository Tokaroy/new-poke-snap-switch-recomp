// SPDX-License-Identifier: MIT
// Screenshot key: a lossless PNG of the frame at the render size (before the present downscale), by default without the game's HUD.
//   F12, View+R3 on a controller, or route token "screenshot" (runtime/hle/input_core.cpp consume_screenshot)
//   ShotOnPresent    nvn_vk.cpp present, once per game frame after the window texture went to TRANSFER_SRC: takes a request; with the
//                    HUD it copies the window texture now, without it arms ShotAfterDraw for the next frame
//   ShotAfterDraw    nvn_draw.cpp, right after the tonemap / AA draws (the point nvn_framegen.cpp takes its HUD-less copy at)
//   ShotAfterSubmit  nvn_vk.cpp present, after the submit: hands the copy to a BelowNormal writer thread, which waits for the GPU and
//                    writes the PNG (the present thread does not wait)
// Folder: SNAP_SHOT_DIR, else setting shot_dir, else <Pictures>\Snap Recomp. HUD: SNAP_SHOT_HUD=1, else setting shot_hud (default off).
// A frame with no 3D scene (menus, loading) has no HUD-less point: after 20 presents the shot is taken with the HUD. A menu that
// still runs the tonemap draw over an empty scene (photo select) gives a blank HUD-less copy, so the frame as shown is copied with it
// and saved instead.
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <functional>
#include <string>

namespace nvn {
struct Host;
struct ReplayCtx;
struct TextureImpl;
void ShotOnPresent(Host* h, VkCommandBuffer cb, TextureImpl* src);
void ShotAfterDraw(ReplayCtx& cx, const char* fragKey12);
void ShotAfterSubmit(Host* h, VkSemaphore timeline, uint64_t value);

// Shared with nvn_hdphoto.cpp (hi-res photo PNGs):
// Writes `px` (rows of w*4 bytes, RGBA when !bgra) as a 24-bit PNG through WIC, to <path>.tmp then renamed. UTF-8 path.
bool ShotWritePng(const std::string& path, const uint8_t* px, uint32_t w, uint32_t h, bool bgra);
// Runs `job` on the BelowNormal writer thread (one thread, jobs in order).
void ShotPost(std::function<void()> job);
// Teardown: ShotDrain waits for every queued job (DestroyQueue, before the queue's timeline semaphore goes); ShotShutdown drains,
// refuses new jobs and joins the writer thread (Host::Shutdown, before the device goes).
void ShotDrain();
void ShotShutdown();
}  // namespace nvn
