// SPDX-License-Identifier: MIT
// Ray tracing track (2026-11-14, rt worker; docs/lighting-rt-plan.md section 4). Everything is behind SNAP_RT=1: with it unset the device,
// the shader modules, the pipelines and every buffer are exactly what they were (no extension, no feature, no hook does anything).
//
//   M0  device: VK_KHR_acceleration_structure + ray_query + deferred_host_operations + EXT_transform_feedback, Vulkan 1.2 bufferDeviceAddress.
//       Buffers the game's memory pools create are NOT touched: only this file's own buffers carry the device-address flags.
//   M1  geometry capture: every vertex shader gets a transform-feedback capture of gl_Position (SPIR-V patch, harmless when no capture
//       is active). A scene opaque draw (1920x1080 DEPTH32F target, depth write on, no blending) is captured by the same draw call that
//       renders it: post-vertex-shader clip positions, expanded to a triangle list, into a persistent arena. A compute pass maps them to
//       world space with the inverse of the camera bank's viewProjection (c3 +0x100); no vertex or index data is read from guest memory.
//   M2  one BLAS per captured draw (persistent cache keyed by draw identity, rebuilt while the draw is visible, kept while it is not) and
//       one TLAS per frame, built at the game's tonemap draw. SNAP_RT_DEBUG=1|2|3 draws a ray-query overlay (see rt_ovl.frag).
//   M3  RT sun shadows (SNAP_RT_SHADOWS, nvn_shadowrt.cpp + the "M3" part of nvn_rt.cpp): the game's shadow-cast pass (the dynamic map's casters: the
//       light-frustum culled set, so off-screen casters are in) is captured by transform feedback with the caster matrix, converted to world space,
//       built into per-frame BLASes (pooled by size class) and one shadow TLAS at the first receiver draw of the frame. Receiver fragment programs get
//       every dynamic-map tap replaced by ray queries toward the sun (hard, or a jittered cone). Sun direction = the caster matrix' depth axis, a runtime value.
//
// M3 switches: SNAP_RT_SHADOWS=0|1|2|3 (off | hard | soft 4 rays | soft 8 rays; also off/hard/soft; setting it implies SNAP_RT=1), SNAP_RT_SHADOW_RAYS (soft ray count),
// SNAP_RT_SHADOW_CONE (soft cone half angle in degrees, 1.2), SNAP_RT_SHADOW_BIAS (ray origin offset toward the sun in world units, 0.02),
// SNAP_RT_SHADOW_MARGIN (shadows past the map's edge, in map widths, 0), SNAP_RT_CAST_MB (cast arena, 64) and SNAP_RT_CAST_BLAS_MB (cast BLAS store, 192).
// SNAP_RT_SHADOWS and the cone, bias and margin are live parameters of the dev loop (ab_capture -Effect SNAP_RT_SHADOWS -Values 0,1).
// Switches: SNAP_RT=1 (master), SNAP_RT_DEBUG=<1|2|3>, SNAP_RT_LOG=<n> (stats line every n presents, default 120 with SNAP_RT_DEBUG),
// SNAP_RT_ARENA_MB (default 384), SNAP_RT_BLAS_MB (512), SNAP_RT_SCRATCH_MB (256), SNAP_RT_TTL (frames a stable draw stays in the TLAS
// after it was last seen, default 900), SNAP_RT_FLIPY=0|1 (clip y to screen y), SNAP_RT_CAPTURE=0 (device features only), SNAP_RT_KEYCB=0|1 (per-object constants in the draw key, default 1), SNAP_RT_REUSE=0|1|2 (keep the BLAS of draws that did not move, default 1; 2 = A/B per log window), SNAP_RT_STATIC_FRAMES (4).
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace nvn { struct Host; struct ReplayCtx; struct TextureImpl; struct QueueImpl; }

namespace snap_rt {

bool Wanted();    // SNAP_RT=1 (or SNAP_RT_SHADOWS set)
bool On();        // the device was created with the ray tracing features
bool Capture();   // On() and SNAP_RT_CAPTURE != 0

// Device setup (nvn_vk.cpp): adds the extensions and chains the feature structs after f11; sets f12.bufferDeviceAddress.
void AppendDeviceExt(VkPhysicalDevice phys, std::vector<const char*>& dext, VkPhysicalDeviceVulkan11Features* f11, VkPhysicalDeviceVulkan12Features* f12);
void DeviceCreated(nvn::Host* h);
void DeviceShutdown(nvn::Host* h);   // device idle: releases everything, also a half-built setup
void QueueDestroyed(nvn::QueueImpl* q);   // after the queue finished, before its timeline semaphore is destroyed (clears the RT slots' completion stamps that name it)
const char* FallbackNotice();   // non-null when RT was asked for but fell back to the raster path this session: a short reason for the F1 menu (SNAP_RT_FORCE_FAIL=ext|setup|shadow|build tests it)

// Vertex module patch (nvn_shader.cpp): decorates gl_Position for transform feedback. Returns true when it changed the module.
bool PatchVertexXfb(std::vector<uint32_t>& spv, const char* key12);

// What the draw replay knows about a draw (nvn_draw.cpp fills it; DrawState is private to that file).
struct DrawInfo {
  const char* vsKey = nullptr;          // vertex stage cache key (hex)
  bool hasGeomTess = false;             // a geometry / tessellation stage is bound: not captured
  VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  uint32_t vertexCount = 0;             // vertices the draw assembles (after the host's index rewrite)
  uint32_t instances = 1;
  uint64_t ibAddr = 0, vbAddr[4] = {};  // guest addresses (identity of the draw)
  int first = 0, base = 0, count = 0;
  bool depthTest = false, depthWrite = false, blend0 = false;
  const uint8_t* cam = nullptr; uint64_t camRange = 0;   // vertex constant bank c3 (PerViewCBuffer) in guest memory
  uint64_t cbHash = 0;                  // hash of the first 256 bytes of every other bound vertex constant bank (per-object constants: tells apart draws of one mesh)
  bool cast = false;                    // a draw of the dynamic shadow map's caster pass (depth-only target, 64-byte caster matrix in vertex slot 9)
  const uint8_t* s9 = nullptr; uint64_t s9Range = 0;   // vertex constant slot 9 in guest memory (the caster matrix of a cast draw)
};

// ---- M3: RT sun shadows -------------------------------------------------------------------------------------------------
int ShadowRays();   // rays per receiver tap in the pipeline being chosen: 0 = the game's map taps (or PCF), 1 hard, N soft. 0 on photo / Evaluation frames
// Push constants of the patched receivers (FRAGMENT stage, offset 0, 128 bytes; the layout is documented in nvn_shadowrt.cpp).
struct ShadowPush { float invR[16]; uint64_t tlas, pad; float L[4], T[4], P[4]; };
static_assert(sizeof(ShadowPush) == 128, "ShadowPush is the receivers' push-constant block");
// Called before each draw whose fragment program is an RT receiver: builds the frame's shadow TLAS on the first call (ends the render pass: *reemit = true), then
// fills `out` from the receiver matrix (floats 0..15 of the fragment stage binding-9 buffer (132 bytes); `recv` may be null).
bool ShadowReceiverPrepare(nvn::ReplayCtx& cx, const uint8_t* recv, uint64_t recvRange, ShadowPush* out, bool* reemit);

// One per draw: Begin right before the vkCmdDraw*, End right after. Does nothing unless the draw is a scene opaque draw being captured.
struct DrawCapture {
  nvn::ReplayCtx* cx = nullptr; bool open = false;
  void Begin(nvn::ReplayCtx& c, const DrawInfo& di);
  void End();
  ~DrawCapture() { End(); }
};

// ---- RT reflections (SNAP_RT_REFLECT=1; the water pass, nvn_ssr.cpp + shaders/water_fx.frag RT_REFL, rt_acc.comp) ------------------------------------------------------
bool ReflectOn();   // asked for (env, then the live dev param) and RT is up and has not failed
const char* ReflectionNotice();   // non-null when the RT part of the reflections fell back to screen-space this session: a short reason for the F1 menu
struct ReflInfo { uint64_t tlas = 0, avg = 0; };   // device addresses the water shader reads: the scene TLAS and the per-instance colour cache
// Called by the water pass outside any render pass with the full-resolution scene colour copy it sampled (sampled layout) and the camera view-projection (row-major, clip = vp * (world, 1)).
// Records the colour cache update. false = nothing to trace against this frame (no TLAS yet, a BLAS was released, photo frame, fallback): the water pass uses the screen-space march alone.
bool ReflectionPass(nvn::ReplayCtx& cx, VkImageView col, VkSampler samp, const double vp[4][4], ReflInfo* out);
// ---- RT ambient occlusion (SNAP_RT_AO=1; the AO pass of nvn_lighting_fx.cpp, shaders/lfx_ao_rt.frag) -----------------------------------------------------------------------------
bool AoOn();   // asked for (env, then the live dev param), RT is up and nothing has failed
const char* AoNotice();   // non-null when the RT AO fell back to GTAO this session: a short reason for the F1 menu
VkShaderModule AoModule(nvn::Host* h);   // the RT AO fragment shader (push block of lfx_ao.frag, q.zw = TLAS address); null if it could not be made (fallback)
bool AoPass(uint64_t* tlas);   // false: use GTAO this frame (photo frame, no TLAS yet); true: *tlas = the scene TLAS device address
// ---- RT bounce light (SNAP_RT_GI=1 together with SNAP_SSGI=1; the gather pass of nvn_lighting_fx.cpp, shaders/lfx_gi_rt.frag) ------------------------------------------------
bool GiOn();   // asked for (env, then the live dev param), RT is up and nothing has failed
const char* GiNotice();   // non-null when the RT gather fell back to the screen-space one this session: a short reason for the F1 menu
VkShaderModule GiModule(nvn::Host* h);   // the RT gather fragment shader (push block: the lfx_gi.frag block plus tlas and avg addresses); null if it could not be made (fallback)
// Same inputs and contract as ReflectionPass: updates the shared colour cache once per frame, returns the TLAS and cache addresses the gather shader needs.
bool GiPass(nvn::ReplayCtx& cx, VkImageView col, VkSampler samp, const double vp[4][4], ReflInfo* out);

// The game's tonemap draw has just been recorded (nvn_draw.cpp, next to AaAfterDraw): convert, build the BLAS/TLAS, draw the overlay.
void OnTonemap(nvn::ReplayCtx& cx, const char* fsKey);
void OnPresent();   // frame bookkeeping (nvn_vk.cpp present)

}  // namespace snap_rt
