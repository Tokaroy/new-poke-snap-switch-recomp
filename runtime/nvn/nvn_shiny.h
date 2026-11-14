// SPDX-License-Identifier: MIT
// Shiny Pokemon mod (docs/shiny-mod-plan.md): per-draw texture twin swap. Off unless SNAP_SHINY=1 (or SNAP_SHINY_TRACE=1 for the log only).
#pragma once
#include <cstdint>
struct VkCommandBuffer_T;
struct ShinyDraw { uint64_t vb0 = 0, vb1 = 0; const void* uboPool[8] = {}; uint64_t uboOff[8] = {}, uboRange[8] = {}; int rtW = 0, rtH = 0, prog = -1; const float* vbf = nullptr; const void* rt0 = nullptr; };   // vbf: host view of vertex stream 0 (guest memory), may be null
namespace nvn { struct TextureImpl; void ShinyBufferInit(uint64_t va, uint64_t size); void ShinyBufferFinalize(uint64_t va, uint64_t size); bool ShinyActive(); TextureImpl* ShinySubstitute(TextureImpl* t, const ShinyDraw& d);
  // M3 photo marks: a 360x240 thumbnail rendered at the shutter while a shiny is on screen; a render target a shiny was drawn into
  // since its last clear (score screen / photo preview); the source of the last GPU->guest photo copy.
  void ShinyThumbCapture(TextureImpl* dst); void ShinyRtClear(const TextureImpl* rt); void ShinyTexGone(const TextureImpl* t); void ShinyCopySrc(const TextureImpl* t); void ShinyCopyTex(const TextureImpl* s, const TextureImpl* d);
  uint32_t ShinyQueryBegin(struct Host* h, VkCommandBuffer_T* cb); void ShinyQueryDiscard(); void ShinyQueryEnd(VkCommandBuffer_T* cb, uint32_t qi); }
// UI badge rects (1920x1080 space) of flagged textures sampled in the last frames; returns the count (host UI, runtime/ui/ui_shiny.cpp).
extern "C" int snap_shiny_badge_rects(float* out4, int max);
// 1 when the last photo-sized copy came from a flagged texture (species in out); frames = how many presents back it may be.
extern "C" int snap_shiny_copy_flag(int frames, char* out, int n);
