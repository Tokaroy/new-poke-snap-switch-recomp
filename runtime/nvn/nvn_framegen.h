// SPDX-License-Identifier: MIT
// AMD FSR frame generation, GPU side (project code, ours). The FidelityFX wrapper is upscale/framegen.cpp; this file owns the images
// and the three places the game frame passes through:
//   1. FgPrepare   (scene pass, upscale.cpp Resolve): motion vectors + depth go to the DLL's prepare dispatch.
//   2. FgAfterDraw (right after the tonemap and AA draws): a raw copy of the 3D scene into the HUD-less ring, taken before the game draws
//      its menus and HUD into the same window texture, so the interface stays out of the interpolation.
//   3. FgBeginPresent / FgRecordGenerate (nvn_vk.cpp present): the DLL interpolates the previous real frame and this one; the
//      generated frame is presented first, then (half a base frame later) the real frame.
// Generated images exist only in the images below and only ever reach the swapchain: no game texture, photo capture or Evaluation
// pass sees them. Frames that run a photo guard (upscale.cpp frameHold) are never prepared, so they are real frames only.
// Off unless SNAP_FRAMEGEN=1 (settings key frame_generation). SNAP_FRAMEGEN_LOG=1 prints base/presented fps once a second,
// SNAP_FRAMEGEN_TRACE=<csv> writes one row per present, SNAP_FRAMEGEN_PANEL=<hz> overrides the panel refresh (tests),
// SNAP_FRAMEGEN_FORCE=1 skips the base-rate gate (tests).
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <functional>

namespace nvn {
struct Host;
struct ReplayCtx;
struct TextureImpl;

bool FgWanted();                       // SNAP_FRAMEGEN=fsr (or 1), read once
void FgDeviceCreated(Host* h);         // loads the DLL and creates the DLL's device context (after the upscaler's DeviceCreated)
void FgDestroy(Host* h);               // device is going away
void FgForgetTexture(const TextureImpl* t);   // DestroyTexture: drops a pending frame keyed by that window texture
const char* FgNotice();                // null while frame generation is fine; once it turned itself off this session, the reason for the F1 menu
void FgFail(const char* why);          // turns frame generation off for this session (a present or swapchain error): one log line, normal frames go on
bool FgForcedFail(const char* stage);  // SNAP_FRAMEGEN_FAIL=init|prepare|generate|present|acquire (tests): true when that stage is to fail
bool FgActive();                       // the rate gate currently lets frames be prepared and generated

// Scene inputs of one frame (upscale.cpp Resolve, recorded on cx.cb after the motion-vector pass).
struct FgScene {
  TextureImpl* display = nullptr;      // the window texture this frame's tonemap draws into (cx.rt[0] at the upscale point)
  VkImage depth = VK_NULL_HANDLE; VkImageView depthView = VK_NULL_HANDLE; VkFormat depthFormat = VK_FORMAT_UNDEFINED;   // DEPTH_STENCIL_READ_ONLY_OPTIMAL
  VkImage mv = VK_NULL_HANDLE; VkImageView mvView = VK_NULL_HANDLE;                                                     // R16G16_SFLOAT, SHADER_READ_ONLY_OPTIMAL
  uint32_t rw = 0, rh = 0;             // render (scene) size
  float jx = 0, jy = 0;                // jitter used by this frame's scene draws, render pixels
  float nearZ = 0.01f, farZ = 1000.0f, fovY = 0.6f;
  float eye[3] = {}, fwd[3] = {0, 0, 1};
  bool cut = false;                    // camera cut: the DLL resets its history
};
bool FgPrepare(ReplayCtx& cx, const FgScene& s);   // true when the frame was prepared (a Generate at its present is owed)
void FgAfterDraw(ReplayCtx& cx, const char* fragKey12);

// Present. FgBeginPresent runs once per game frame, after the frame limiter; true = a generated frame is presented first (call
// FgRecordGenerate on that present's command buffer once src is in TRANSFER_SRC_OPTIMAL, blit the returned image, then present src
// again with FgSleepHalf in between). The pending frame of src is consumed either way.
bool FgBeginPresent(Host* h, TextureImpl* src, bool hdr);
// Records the interpolation into the present command buffer; returns the sRGB image (TRANSFER_SRC_OPTIMAL, same size as src) to blit,
// or VK_NULL_HANDLE when nothing is to be shown (the real frame is then presented alone).
VkImage FgRecordGenerate(Host* h, VkCommandBuffer cb, TextureImpl* src);
void FgSleepHalf();                    // waits until half a base frame after FgBeginPresent (SNAP_FRAMEGEN_THREAD=0: the old path that blocks the game thread)
void FgNotePresent(bool generated);    // every vkQueuePresentKHR (generated = the interpolated one): counters, SNAP_FRAMEGEN_LOG / _TRACE

// Presenter thread (2026-11-16). A game frame with frame generation presents twice. Timing the second present with a sleep on the game
// thread cost half a base frame per frame, so with no frame limit the presented rate fell back to the plain rate (the SavageWindRecomp
// finding). Both vkQueuePresentKHR calls of such a frame are now queued here with a due time and made by this thread; the game thread
// records and submits both frames and moves on. Order is kept: a frame that is not generated waits for the queue to drain first.
bool FgThreaded();                                  // the presenter thread is in use (frame generation on, SNAP_FRAMEGEN_THREAD not 0)
void FgPresentAt(std::function<void()> present, int64_t dueNs);   // runs `present` on the presenter thread at dueNs (steady clock ns)
void FgDrainPresents();                             // waits until every queued present was made; call with the queue mutex NOT held
void FgWaitQueued(uint32_t maxQueued);              // waits until at most maxQueued presents are queued or in progress (each holds an acquired image); queue mutex NOT held
uint32_t FgQueued();                                 // presents queued or in progress on the presenter thread (diagnostics)
int64_t FgReleaseNs();                              // steady-clock ns of the last FgBeginPresent (the game frame's present time)
double FgBaseDt();                                  // smoothed base frame time in seconds (0 until measured)
int64_t FgNowNs();
}  // namespace nvn
