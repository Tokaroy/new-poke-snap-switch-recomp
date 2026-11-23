// SPDX-License-Identifier: MIT
// Smooth camera (project code, ours): camera render interpolation above 60 fps, display only.
//
// The game keeps simulating and rendering at its own rate (60 fps by default). On a faster display every presented frame shows the
// newest game frame re-projected to a camera between the last two game cameras, so turning and panning move on every refresh
// instead of in 60 Hz steps. Pokemon, particles and the interface still move in game-frame steps (they are not re-simulated).
//
// Data flow (all in the window's own frame, nothing the game reads is written):
//   1. ScPrepare   (upscale.cpp Resolve, the scene's upscale point): the camera of this game frame (PerViewCBuffer c3) and a copy of its
//                  scene depth into a host image.
//   2. ScAfterDraw (nvn_draw.cpp, right after the tonemap and AA draws): a raw copy of the finished 3D scene before the game draws its
//                  menus and HUD (as frame generation does).
//   3. ScBeginPresent / ScRecordPass (nvn_vk.cpp PresentWindowOnce): the frame is presented as 1 to 8 passes on the display clock, one
//                  display period apart and one game frame late; each pass is the re-projection for its point in time (the in-between
//                  camera), with the interface pixels of the finished frame copied unchanged. The presenter thread of
//                  nvn_framegen.cpp makes the vkQueuePresentKHR calls at their due times.
// Photos and scoring: the game's own passes (photo targets, Evaluation, saved JPEGs) never see these images; they exist only in the
// images below and the swapchain, and the game's simulation, timing and frame count are untouched. Photo frames (the upscale photo
// guard) are presented as they are, without re-projection.
//
// Switches: SNAP_SMOOTH_CAMERA=1 (settings key smooth_camera, flag --smooth-camera on|off; default off; restart). Off while frame
// generation is on and with an HDR swapchain. Gate: on while the present rate is at least 1.5x the game rate (off below 1.35x).
// Test aids: SNAP_SMOOTH_CAMERA_HZ=<hz> present rate (default the monitor refresh, lowered by the frame rate limit);
// SNAP_SMOOTH_CAMERA_PAUSE=1 keeps the gate shut (plain presents, still traced); SNAP_SMOOTH_CAMERA_LOG=1 one stderr line a second;
// SNAP_SMOOTH_CAMERA_TEST_ALPHA=<0..1> one re-projected pass at that alpha per frame, then the frame (for frame dumps);
// SNAP_SMOOTH_CAMERA_TRACE=<csv> one row per vkQueuePresentKHR with the shown camera (smoothness evidence).
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

namespace nvn {
struct Host;
struct QueueImpl;
struct ReplayCtx;
struct TextureImpl;

bool ScWanted();                         // SNAP_SMOOTH_CAMERA=1 and frame generation off, read once
void ScDeviceCreated(Host* h);
void ScDestroy(Host* h);                 // stops the presenter thread too (before the device goes)
void ScForgetTexture(const TextureImpl* t);
uint32_t ScExtraImages();                // swapchain images to add: the presenter holds one frame's passes until their due times

// Scene of one game frame (upscale.cpp Resolve, recorded on cx.cb; depth in DEPTH_STENCIL_READ_ONLY_OPTIMAL).
struct ScScene {
  TextureImpl* display = nullptr;        // the window texture this frame's tonemap draws into
  VkImageView depthView = VK_NULL_HANDLE;
  uint32_t rw = 0, rh = 0;               // depth (render) size
  double vp[4][4] = {};                  // viewProjection, row-major (clip = vp * world)
  double proj[4][4] = {};                // projection, row-major
  double eye[3] = {};                    // cameraPosition
  bool cut = false;                      // camera cut or history reset: no in-between frames from the previous camera
};
void ScPrepare(ReplayCtx& cx, const ScScene& s);
void ScAfterDraw(ReplayCtx& cx, const char* fragKey12);

// Present plan of one game frame (filled by ScBeginPresent).
struct ScPlan {
  static constexpr int kMax = 8;
  int n = 1;                             // passes (the last one also carries the real-frame side jobs: screenshot, dumps)
  float alpha[kMax] = {1, 1, 1, 1, 1, 1, 1, 1};   // camera position between the previous game camera (0) and this one (1)
  bool warp[kMax] = {};                  // re-projected pass (else the finished frame as it is)
  int64_t due[kMax] = {};                // steady-clock ns the present is made at
};
// After the frame limiter, once per game frame. true: the passes go through the presenter thread at plan->due (presents of earlier
// frames may still be queued). false: present the frame now as usual (drain the presenter first).
bool ScBeginPresent(Host* h, TextureImpl* src, bool hdr, ScPlan* plan);

// What a present showed (trace and counters), filled by ScRecordPass and handed to ScNotePresent.
struct ScShown {
  bool on = false, warped = false, haveCam = false;
  int pass = 0, passes = 1;
  float alpha = 1;
  uint64_t frame = 0;
  double yaw = 0, pitch = 0, eye[3] = {};
  int64_t due = 0;
};
// Records pass `pass` of the plan into the present command buffer (src in TRANSFER_SRC_OPTIMAL, queue mutex held). Returns the image to
// blit instead of src (TRANSFER_SRC_OPTIMAL, src's size and format) or VK_NULL_HANDLE to blit src. Also fills *shown.
VkImage ScRecordPass(Host* h, QueueImpl* q, VkCommandBuffer cb, TextureImpl* src, const ScPlan& plan, int pass, ScShown* shown);
void ScNotePresent(const ScShown& s);    // at each vkQueuePresentKHR (any thread)
}  // namespace nvn
