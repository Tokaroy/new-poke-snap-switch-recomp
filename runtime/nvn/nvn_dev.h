// SPDX-License-Identifier: MIT
// Snap Recomp developer loop: one small API shared by the renderer sessions (project code, ours, no game data).
// Everything here is OFF unless SNAP_DEV=1. With SNAP_DEV unset each call below is one cached-bool branch (the inline wrappers) and no
// state exists: no thread, no file, no allocation. Full description and the control-channel protocol: docs/devloop.md.
//
//   live-tunable values   dev_param_f/i/b("SNAP_X", &g_x, min, max, default)   shows in the F1 "Tuning" tab, settable from a script
//   live shaders          dev_spv("lfx_ao", kLfxAoFrag, sizeof kLfxAoFrag)      SNAP_SHADER_DIR/<name>.frag|.comp|.spv wins over the embedded copy
//   GPU pass timing       dev_pass_begin(cb, "my pass") ... dev_pass_end(cb, "my pass")   ms per pass in the corner overlay and in ctl `passes`
//   control channel       build/ctl/<pid>.cmd (commands in), <pid>.out (answers), <pid>.passes (ms per pass, rewritten each second)
//
// Naming rule: a param's name is the env var that sets the same value ("SNAP_LIGHT_AO_RADIUS"). The panel's "copy as env line" prints
// NAME=value, and a set env var seeds the value at registration. Thread rules: params are plain loads and stores of 32-bit values, read them
// every frame (never cache them across frames) and a change is seen the next frame. Registered pointers must outlive the process (statics,
// globals, never stack or heap that is freed). Shader and pass calls come from the render / present threads.
#pragma once
#include <cstddef>
#include <cstdint>

// same typedef as <vulkan/vulkan.h> (repeating a typedef to the same type is legal C++), so this header needs no Vulkan include
typedef struct VkCommandBuffer_T* VkCommandBuffer;

// ---- on/off ------------------------------------------------------------------------------------------------------------
bool dev_env_on();                                       // nvn_dev.cpp: SNAP_DEV is set to something other than 0
inline bool dev_on() { static const bool v = dev_env_on(); return v; }

// ---- live-tunable values -----------------------------------------------------------------------------------------------
// Register once (any time, repeat calls with the same name and pointer are fine). *p keeps whatever value the caller gave it, unless the env var
// `name` is set, which wins and is written into *p (clamped to [mn,mx]). The panel and ctl `set` then write *p; reset goes back to `def`.
void dev_param_f_impl(const char* name, float* p, float mn, float mx, float def);
void dev_param_i_impl(const char* name, int* p, int mn, int mx, int def);
void dev_param_b_impl(const char* name, bool* p, bool def);
inline void dev_param_f(const char* name, float* p, float mn, float mx, float def) { if (dev_on()) dev_param_f_impl(name, p, mn, mx, def); }
inline void dev_param_i(const char* name, int* p, int mn, int mx, int def) { if (dev_on()) dev_param_i_impl(name, p, mn, mx, def); }
inline void dev_param_b(const char* name, bool* p, bool def) { if (dev_on()) dev_param_b_impl(name, p, def); }
inline void dev_param_b(const char* name, bool* p, int, int, bool def) { if (dev_on()) dev_param_b_impl(name, p, def); }   // 5-arg form, min/max ignored

// Drop-in for an `EnvF("SNAP_X", def)` that is read every frame: with SNAP_DEV off it is exactly getenv + atof (what EnvF does), with it on
// the value lives in a registered param (initial value from the env var if set, else def) and the panel / ctl change it. Same for ints.
float dev_envf(const char* env, float def, float mn, float mx);
int dev_envi(const char* env, int def, int mn, int mx);

// ---- shaders -----------------------------------------------------------------------------------------------------------
// The SPIR-V for shader `name`. With SNAP_DEV off this is the embedded copy, nothing else. With SNAP_DEV=1 and SNAP_SHADER_DIR set:
//   <dir>/<name>.frag|.comp|.vert (GLSL source) is compiled with the pinned glslangValidator (tools/vulkan_sdk, or SNAP_GLSLANG) and used when
//   it compiles; otherwise <dir>/<name>.spv; otherwise the embedded copy. `embedded_bytes` is a byte size (pass sizeof array).
// The returned words stay valid for the life of the process. A GLSL edit while the game runs is recompiled in the background and applied at the
// next frame boundary: dev_spv_changed() then returns true once for each consumer and a following dev_spv() returns the new words. A compile error
// keeps the previous shader and is shown on screen and logged.
struct DevSpv { const uint32_t* words; size_t bytes; };
DevSpv dev_spv_impl(const char* name, const uint32_t* embedded, size_t embedded_bytes);
inline DevSpv dev_spv(const char* name, const uint32_t* embedded, size_t embedded_bytes) {
  if (!dev_on()) return DevSpv{embedded, embedded_bytes};
  return dev_spv_impl(name, embedded, embedded_bytes);
}
bool dev_spv_changed_impl(const char* name, uint32_t* seen);
// Per consumer: keep a `uint32_t seen = 0;` next to the pipeline; call this where the pipeline is used each frame. True when the shader was reloaded since
// `seen` was last updated (it is updated here), so rebuild the pipelines that use `name` then. The first call after startup is false unless a reload happened.
inline bool dev_spv_changed(const char* name, uint32_t* seen) { return dev_on() && dev_spv_changed_impl(name, seen); }

// ---- GPU pass timing ---------------------------------------------------------------------------------------------------
// Timestamps in the command buffer being recorded for the frame (the one the renderer replays into; calls with any other cb are ignored). The
// time from dev_pass_begin to the next mark in that command buffer (dev_pass_end, another begin, or one of the renderer's own SNAP_GPU_PASSES
// labels) is booked to `name`. Averaged over 1 s in the overlay; summed per frame when a pass runs several times a frame.
// Only active while the overlay or `timing on` is on; otherwise a no-op beyond the dev_on() branch.
void dev_pass_begin_impl(VkCommandBuffer cb, const char* name);
void dev_pass_end_impl(VkCommandBuffer cb, const char* name);
inline void dev_pass_begin(VkCommandBuffer cb, const char* name) { if (dev_on()) dev_pass_begin_impl(cb, name); }
inline void dev_pass_end(VkCommandBuffer cb, const char* name) { if (dev_on()) dev_pass_end_impl(cb, name); }

// ---- called by the renderer / UI plumbing (not by effects) -------------------------------------------------------------
void dev_frame(uint64_t present);   // once per present, present thread: control channel, shader watcher, 1 s pass averages
void dev_pass_sample(const char* label, double ms);   // nvn_perf.cpp: one collected segment (label of a mark, GPU ms)
void dev_gpu_frame(double busy_ms);                   // nvn_perf.cpp: end of one collected frame
bool dev_timing_wanted();                             // overlay or ctl asked for GPU timestamps
bool dev_renderer_marks();                            // also book the renderer's own SNAP_GPU_PASSES marks (ctl `renderer on`, SNAP_DEV_RENDERER_MARKS=1)
void dev_renderer_set(bool on);
void dev_timing_apply(bool on);                       // nvn_perf.cpp provides: turns the timestamp marks on or off

struct DevParamView { const char* name; char kind; double value, mn, mx, def; };   // kind 'f', 'i' or 'b'
int dev_param_list(DevParamView* out, int max);       // snapshot, registration order
bool dev_param_set(const char* name, double v);       // clamped; false when unknown
bool dev_param_reset(const char* name);
struct DevPassStat { char name[64]; double ms; };
int dev_pass_stats(DevPassStat* out, int max);        // 1 s averages, largest first
double dev_gpu_busy();                                // 1 s average of GPU busy ms per frame, -1 before the first window
int dev_shader_errors(char* out, int out_len);        // compile errors of the shaders that failed their last reload ("" when none), returns count
void dev_shader_reload_all();                         // recompile every watched shader now
bool dev_overlay_get();
void dev_overlay_set(bool on);
const char* dev_ctl_dir();                            // directory of the control files
