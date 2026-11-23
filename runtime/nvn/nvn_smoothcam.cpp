// SPDX-License-Identifier: MIT
// Smooth camera: camera render interpolation above 60 fps (project code, ours). See nvn_smoothcam.h for the data flow.
//
// Images (device-local, created at the first prepared frame, rebuilt on a size change; two of each so a frame's data is never
// overwritten by the next frame's replay before its passes ran):
//   scene[2]  UNORM, display size: raw copies of the sRGB window texture right after the tonemap/AA draw (before the game's interface)
//   depth[2]  R32F, render size: this frame's scene depth
//   pres      UNORM, display size: raw copy of the finished window texture at present (interface included)
//   out       RGBA8 UNORM storage: the re-projected frame
//   show      the window texture's format: raw copy of `out`, the thing the present blits
// Raw copies between sRGB and UNORM twins keep the stored (gamma) bytes, so no colour conversion happens anywhere.
//
// Timing: game frame N is released by the frame limiter at R_N. Its passes are due on a grid of display periods P, one game frame
// later: a grid time t shows the camera of time t - D (D = the smoothed game frame time + 1 ms), between camera N-1 (at R_{N-1}) and
// camera N (at R_N): alpha = (t - D - R_{N-1}) / (R_N - R_{N-1}). Every grid time is used once, so the display gets one new picture per
// period whatever the ratio of the two rates (2.4 per game frame at 144 Hz).
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "nvn_smoothcam.h"
#include "nvn_aa.h"
#include "nvn_framegen.h"
#include "nvn_internal.h"
#include "nvn_pace.h"
#include "nvn_smoothcam_spv.h"
#include "smoothcam_core.h"

extern "C" int snap_window_frame_cap();   // host/window_opts.cpp (weak default in nvn_vk.cpp)

namespace nvn {
namespace {

int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
bool EnvOn(const char* n) { const char* e = getenv(n); return e && e[0] && e[0] != '0'; }

using snap_smoothcam::M4;
using snap_smoothcam::Invert;
using snap_smoothcam::Mul;

struct Cam {
  bool ok = false;
  M4 vp{}, proj{};
  double eye[3] = {};
  uint64_t seq = 0;   // g_presentCount while the frame was replayed
  int slot = -1;      // image slot (scene / depth)
  bool sceneCopied = false, cut = false;
};

struct Img { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkImageLayout lay = VK_IMAGE_LAYOUT_UNDEFINED; };

constexpr int kRing = 24;   // warp descriptor sets (up to 8 passes a frame, gated by the queue timeline)

struct State {
  std::mutex mu;
  Host* h = nullptr;
  bool ready = false, failed = false, active = false;
  // pipelines
  VkSampler linSamp = VK_NULL_HANDLE, nearSamp = VK_NULL_HANDLE;
  VkDescriptorSetLayout dslDepth = VK_NULL_HANDLE, dslWarp = VK_NULL_HANDLE;
  VkPipelineLayout plDepth = VK_NULL_HANDLE, plWarp = VK_NULL_HANDLE;
  VkPipeline pipeDepth = VK_NULL_HANDLE, pipeWarp = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet set[kRing] = {}; uint64_t setValue[kRing] = {}; int setNext = 0;
  // images
  uint32_t dw = 0, dh = 0, rw = 0, rh = 0; VkFormat dispFmt = VK_FORMAT_UNDEFINED;
  Img scene[2], depth[2], pres, out, show;
  int slotNext = 0;
  // frames
  std::unordered_map<const TextureImpl*, Cam> pend;
  Cam cur, prev;
  bool presCopied = false;
  // clock
  int64_t lastRel = 0, gridNext = 0; double dtEma = 0;
  uint64_t frames = 0;
  // stats
  uint64_t presents = 0, warped = 0, gameFrames = 0, prepared = 0, noScene = 0, cuts = 0, notConsecutive = 0, lateTicks = 0, tmDraws = 0, tmNotPending = 0, tmSize = 0;
  uint64_t winPresents = 0, winWarped = 0, winFrames = 0; int64_t winNs = 0;
} S;

std::mutex g_traceMu; FILE* g_trace = nullptr; int64_t g_traceT0 = 0;

VkFormat Unorm(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
  }
}

void Bar(VkCommandBuffer cb, Img& i, VkImageLayout to, VkImageAspectFlags asp = VK_IMAGE_ASPECT_COLOR_BIT) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = i.lay; b.newLayout = to; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = i.img; b.subresourceRange = {asp, 0, 1, 0, 1};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cb, &di);
  i.lay = to;
}

bool MakeImg(Host* h, Img& o, uint32_t w, uint32_t hh, VkFormat fmt, VkImageUsageFlags usage) {
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {w, hh, 1}; ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(h->device, &ci, nullptr, &o.img) != VK_SUCCESS) return false;
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(h->device, o.img, &mr);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size; ai.memoryTypeIndex = h->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(h->device, &ai, nullptr, &o.mem) != VK_SUCCESS || vkBindImageMemory(h->device, o.img, o.mem, 0) != VK_SUCCESS) return false;
  VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vc.image = o.img; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = fmt; vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  o.lay = VK_IMAGE_LAYOUT_UNDEFINED;
  return vkCreateImageView(h->device, &vc, nullptr, &o.view) == VK_SUCCESS;
}
void FreeImg(Host* h, Img& o) {
  if (o.view) vkDestroyImageView(h->device, o.view, nullptr);
  if (o.img) vkDestroyImage(h->device, o.img, nullptr);
  if (o.mem) vkFreeMemory(h->device, o.mem, nullptr);
  o = Img{};
}
void FreeImages(Host* h) {
  for (Img& i : S.scene) FreeImg(h, i);
  for (Img& i : S.depth) FreeImg(h, i);
  FreeImg(h, S.pres); FreeImg(h, S.out); FreeImg(h, S.show);
  S.dw = S.dh = S.rw = S.rh = 0; S.dispFmt = VK_FORMAT_UNDEFINED;
}

void Fail(const char* why) {
  if (S.failed) return;
  S.failed = true; S.active = false;
  fprintf(stderr, "[smoothcam] %s: smooth camera off for this run\n", why);
}

// Caller holds S.mu.
bool EnsureImages(uint32_t dw, uint32_t dh, uint32_t rw, uint32_t rh, VkFormat disp) {
  if (S.dw == dw && S.dh == dh && S.rw == rw && S.rh == rh && S.dispFmt == disp && S.out.img) return true;
  Host* h = S.h;
  if (S.dw) vkDeviceWaitIdle(h->device);   // a resize: rare, and nothing recorded may still read the old images
  FreeImages(h);
  S.pend.clear(); S.cur = Cam{}; S.prev = Cam{};
  const VkFormat un = Unorm(disp);
  const VkImageUsageFlags colorIn = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  bool ok = true;
  for (Img& i : S.scene) ok = ok && MakeImg(h, i, dw, dh, un, colorIn);
  for (Img& i : S.depth) ok = ok && MakeImg(h, i, rw, rh, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
  ok = ok && MakeImg(h, S.pres, dw, dh, un, colorIn);
  ok = ok && MakeImg(h, S.out, dw, dh, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  ok = ok && MakeImg(h, S.show, dw, dh, disp, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  if (!ok) { FreeImages(h); Fail("image creation failed"); return false; }
  S.dw = dw; S.dh = dh; S.rw = rw; S.rh = rh; S.dispFmt = disp;
  fprintf(stderr, "[smoothcam] images: display %ux%u, depth %ux%u\n", dw, dh, rw, rh);
  return true;
}

bool MakePipe(Host* h, const uint32_t* code, size_t bytes, int nSampled, uint32_t pcBytes, VkDescriptorSetLayout* dsl, VkPipelineLayout* pl, VkPipeline* pipe) {
  VkDevice d = h->device;
  VkDescriptorSetLayoutBinding b[4] = {};
  for (int i = 0; i <= nSampled; i++) {
    b[i].binding = (uint32_t)i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[i].descriptorType = i < nSampled ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  }
  VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount = (uint32_t)nSampled + 1; dl.pBindings = b;
  if (vkCreateDescriptorSetLayout(d, &dl, nullptr, dsl) != VK_SUCCESS) return false;
  VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, pcBytes};
  VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  li.setLayoutCount = 1; li.pSetLayouts = dsl; li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
  if (vkCreatePipelineLayout(d, &li, nullptr, pl) != VK_SUCCESS) return false;
  VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; mi.codeSize = bytes; mi.pCode = code;
  VkShaderModule mod = VK_NULL_HANDLE;
  if (vkCreateShaderModule(d, &mi, nullptr, &mod) != VK_SUCCESS) return false;
  VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
  ci.layout = *pl;
  const VkResult r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, pipe);
  vkDestroyShaderModule(d, mod, nullptr);
  return r == VK_SUCCESS;
}

bool Setup(Host* h) {
  VkDevice d = h->device;
  VkFormatProperties fp{}; vkGetPhysicalDeviceFormatProperties(h->phys, VK_FORMAT_R8G8B8A8_UNORM, &fp);
  if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return false;
  vkGetPhysicalDeviceFormatProperties(h->phys, VK_FORMAT_R32_SFLOAT, &fp);
  if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return false;
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;
  if (vkCreateSampler(d, &si, nullptr, &S.nearSamp) != VK_SUCCESS) return false;
  si.magFilter = si.minFilter = VK_FILTER_LINEAR;
  if (vkCreateSampler(d, &si, nullptr, &S.linSamp) != VK_SUCCESS) return false;
  if (!MakePipe(h, kScDepthSpv, sizeof kScDepthSpv, 1, 8, &S.dslDepth, &S.plDepth, &S.pipeDepth)) return false;
  if (!MakePipe(h, kScWarpSpv, sizeof kScWarpSpv, 3, 96, &S.dslWarp, &S.plWarp, &S.pipeWarp)) return false;
  VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kRing * 3}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kRing}};
  VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets = kRing; dp.poolSizeCount = 2; dp.pPoolSizes = ps;
  if (vkCreateDescriptorPool(d, &dp, nullptr, &S.pool) != VK_SUCCESS) return false;
  VkDescriptorSetLayout layouts[kRing];
  for (auto& l : layouts) l = S.dslWarp;
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = S.pool; ai.descriptorSetCount = kRing; ai.pSetLayouts = layouts;
  return vkAllocateDescriptorSets(d, &ai, S.set) == VK_SUCCESS;
}

double PresentHz() {
  static const double env = [] { const char* e = getenv("SNAP_SMOOTH_CAMERA_HZ"); return e ? atof(e) : 0.0; }();
  if (env > 0) return env;   // test aid: wins over the frame rate limit too
  double hz = snap_pace::PanelHz();
  if (const int cap = snap_window_frame_cap(); cap > 0 && hz > 0) hz = std::min(hz, (double)cap);
  return hz;
}

snap_smoothcam::Camera AsCamera(const Cam& c) { snap_smoothcam::Camera k; k.vp = c.vp; k.proj = c.proj; for (int i = 0; i < 3; i++) k.eye[i] = c.eye[i]; return k; }
using snap_smoothcam::Angles;

bool LogOn() { static const bool f = EnvOn("SNAP_SMOOTH_CAMERA_LOG"); return f; }
}  // namespace

// a save state was loaded (runtime/nvn/nvn_state.cpp hook point): the camera jumped
extern "C" void snap_smoothcam_state_loaded() { std::lock_guard<std::mutex> lk(S.mu); S.prev = Cam{}; }

bool ScWanted() {
  static const bool w = [] {
    if (!EnvOn("SNAP_SMOOTH_CAMERA")) return false;
    if (FgWanted()) { fprintf(stderr, "[smoothcam] frame generation is on: smooth camera stays off\n"); return false; }
    return true;
  }();
  return w;
}

uint32_t ScExtraImages() { return ScWanted() ? 5u : 0u; }

void ScDeviceCreated(Host* h) {
  if (!ScWanted()) return;
  S.h = h;
  if (!Setup(h)) { Fail("pipeline setup failed"); return; }
  S.ready = true;
  if (const char* t = getenv("SNAP_SMOOTH_CAMERA_TRACE")) if (*t) {
    std::lock_guard<std::mutex> lk(g_traceMu);
    g_trace = fopen(t, "w");
    if (g_trace) fprintf(g_trace, "t_ms,frame,pass,passes,alpha,warped,active,yaw_deg,pitch_deg,eye_x,eye_y,eye_z,late_ms\n");
  }
  fprintf(stderr, "[smoothcam] ready (present rate %.1f Hz now%s)\n", PresentHz(), EnvOn("SNAP_SMOOTH_CAMERA_PAUSE") ? ", paused by SNAP_SMOOTH_CAMERA_PAUSE" : "");
}

void ScDestroy(Host* h) {
  if (!S.h || S.h != h) return;
  FgPresenterShutdown();   // no queued present may outlive the swapchain or the device
  snap_pace::SetPresenterOwned(false);
  if (S.presents)
    fprintf(stderr, "[smoothcam] summary: %llu game frames, %llu presents, %llu re-projected, %llu prepared, %llu without a scene copy, %llu camera cuts, %llu not consecutive, %llu late grid points\n",
            (unsigned long long)S.gameFrames, (unsigned long long)S.presents, (unsigned long long)S.warped, (unsigned long long)S.prepared, (unsigned long long)S.noScene,
            (unsigned long long)S.cuts, (unsigned long long)S.notConsecutive, (unsigned long long)S.lateTicks);
  std::lock_guard<std::mutex> lk(S.mu);
  VkDevice d = h->device;
  vkDeviceWaitIdle(d);
  FreeImages(h);
  if (S.pipeDepth) vkDestroyPipeline(d, S.pipeDepth, nullptr);
  if (S.pipeWarp) vkDestroyPipeline(d, S.pipeWarp, nullptr);
  if (S.plDepth) vkDestroyPipelineLayout(d, S.plDepth, nullptr);
  if (S.plWarp) vkDestroyPipelineLayout(d, S.plWarp, nullptr);
  if (S.dslDepth) vkDestroyDescriptorSetLayout(d, S.dslDepth, nullptr);
  if (S.dslWarp) vkDestroyDescriptorSetLayout(d, S.dslWarp, nullptr);
  if (S.pool) vkDestroyDescriptorPool(d, S.pool, nullptr);
  if (S.linSamp) vkDestroySampler(d, S.linSamp, nullptr);
  if (S.nearSamp) vkDestroySampler(d, S.nearSamp, nullptr);
  S.pend.clear();
  S.pipeDepth = S.pipeWarp = VK_NULL_HANDLE; S.plDepth = S.plWarp = VK_NULL_HANDLE; S.dslDepth = S.dslWarp = VK_NULL_HANDLE;
  S.pool = VK_NULL_HANDLE; S.linSamp = S.nearSamp = VK_NULL_HANDLE;
  S.ready = false; S.h = nullptr;
  std::lock_guard<std::mutex> tl(g_traceMu);
  if (g_trace) { fclose(g_trace); g_trace = nullptr; }
}

void ScForgetTexture(const TextureImpl* t) {
  if (!S.ready) return;
  std::lock_guard<std::mutex> lk(S.mu);
  S.pend.erase(t);
}

void ScPrepare(ReplayCtx& cx, const ScScene& s) {
  if (!S.ready || S.failed || !s.display || !s.display->fi || !s.display->image || !s.depthView || !s.rw || !s.rh) return;
  std::lock_guard<std::mutex> lk(S.mu);
  if (Unorm(s.display->fi->vk) == VK_FORMAT_UNDEFINED) {
    static bool told = false;
    if (!told) { told = true; fprintf(stderr, "[smoothcam] window texture format %d is not RGBA8: frames are presented as they are\n", (int)s.display->fi->vk); }
    return;
  }
  if (!EnsureImages(s.display->HW(), s.display->HH(), s.rw, s.rh, s.display->fi->vk)) return;
  const int slot = S.slotNext; S.slotNext ^= 1;
  Img& dimg = S.depth[slot];
  // this frame's depth into the host image
  VkDescriptorSet set = VK_NULL_HANDLE;
  for (int attempt = 0; attempt < 2 && !set; attempt++) {
    if (cx.pools->empty() || attempt == 1) cx.pools->push_back(AcquireDescriptorPool(cx.q));
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = cx.pools->back(); ai.descriptorSetCount = 1; ai.pSetLayouts = &S.dslDepth;
    if (vkAllocateDescriptorSets(cx.h->device, &ai, &set) != VK_SUCCESS) set = VK_NULL_HANDLE;
  }
  if (!set) return;
  VkDescriptorImageInfo ii[2] = {{S.nearSamp, s.depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}, {VK_NULL_HANDLE, dimg.view, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
  for (int i = 0; i < 2; i++) {
    wr[i].dstSet = set; wr[i].dstBinding = (uint32_t)i; wr[i].descriptorCount = 1; wr[i].pImageInfo = &ii[i];
    wr[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  }
  vkUpdateDescriptorSets(cx.h->device, 2, wr, 0, nullptr);
  VkCommandBuffer cb = cx.cb;
  Bar(cb, dimg, VK_IMAGE_LAYOUT_GENERAL);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.pipeDepth);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.plDepth, 0, 1, &set, 0, nullptr);
  const int32_t pc[2] = {(int32_t)s.rw, (int32_t)s.rh};
  vkCmdPushConstants(cb, S.plDepth, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
  vkCmdDispatch(cb, (s.rw + 7) / 8, (s.rh + 7) / 8, 1);
  Bar(cb, dimg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  Cam c;
  c.ok = true; c.seq = (uint64_t)g_presentCount; c.slot = slot; c.cut = s.cut;
  memcpy(c.vp.m, s.vp, sizeof c.vp.m); memcpy(c.proj.m, s.proj, sizeof c.proj.m); memcpy(c.eye, s.eye, sizeof c.eye);
  S.pend[s.display] = c;
  S.prepared++;
}

void ScAfterDraw(ReplayCtx& cx, const char* key) {
  if (!S.ready) return;
  TextureImpl* t = cx.rt[0];
  if (!t || !t->image || !t->fi || !AaIsTonemapKey(key)) return;
  std::lock_guard<std::mutex> lk(S.mu);
  S.tmDraws++;
  auto it = S.pend.find(t);
  if (it == S.pend.end() || S.failed) { S.tmNotPending++; return; }
  SubRange rr;
  t->RenderView(cx.rtHasView[0] ? &cx.rtView[0] : nullptr, &rr);
  const uint32_t w = t->LevelW(rr.lb), hh = t->LevelH(rr.lb);
  if (rr.yn != 1 || w != S.dw || hh != S.dh) { S.tmSize++; return; }
  EndRendering(cx);
  Img& sc = S.scene[it->second.slot];
  VkCommandBuffer cb = cx.cb;
  TransitionRange(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rr);
  Bar(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkImageCopy cp{}; cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, rr.lb, rr.yb, 1}; cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; cp.extent = {w, hh, 1};
  vkCmdCopyImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
  Bar(cb, sc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  TransitionRange(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, rr);
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cb, &di);
  it->second.sceneCopied = true;   // a second tonemap-keyed draw of the same frame copies again into the same slot
}

bool ScBeginPresent(Host*, TextureImpl* src, bool hdr, ScPlan* plan) {
  *plan = ScPlan{};
  if (!S.ready) return false;
  std::lock_guard<std::mutex> lk(S.mu);
  const int64_t now = NowNs();
  const int64_t B = S.lastRel ? now - S.lastRel : 0;
  if (B > 2000000 && B < 250000000) { const double d = (double)B * 1e-9; S.dtEma = S.dtEma > 0 ? S.dtEma + (d - S.dtEma) * 0.1 : d; }
  S.gameFrames++; S.winFrames++;
  // this frame's camera (prepared during its replay) and the previous one
  Cam c;
  auto it = S.pend.find(src);
  if (it != S.pend.end()) { c = it->second; S.pend.erase(it); }
  if (c.ok && c.seq + 1 != (uint64_t)g_presentCount) c = Cam{};   // left over from an earlier frame of this window texture
  if (S.cur.ok) S.prev = S.cur;
  else S.prev = Cam{};
  S.cur = c;
  S.presCopied = false;
  // gate
  const double hz = PresentHz(), base = S.dtEma > 0 ? 1.0 / S.dtEma : 0.0;
  const bool was = S.active;
  bool want = !S.failed && hz > 0 && base > 0 && hz >= base * (was ? 1.35 : 1.5) && !hdr;
  if (EnvOn("SNAP_SMOOTH_CAMERA_PAUSE")) want = false;
  S.active = want;
  if (want != was) {
    fprintf(stderr, "[smoothcam] %s (game %.1f fps, present rate %.1f Hz%s)\n", want ? "on" : "off", base, hz, hdr ? ", HDR output" : "");
    snap_pace::SetPresenterOwned(want);
    S.gridNext = 0;
  }
  const int64_t lastRel = S.lastRel;
  S.lastRel = now;
  if (!want) return false;
  // grid points for this frame: t - D in (R_{N-1}, R_N] (smoothcam_core.h PlanFrame)
  const int64_t P = (int64_t)(1e9 / hz);
  const int64_t D = std::clamp<int64_t>((int64_t)(S.dtEma * 1e9) + 1000000, 4000000, 60000000);
  const bool warpOk = S.cur.ok && S.cur.sceneCopied && S.prev.ok && S.prev.seq + 1 == S.cur.seq && !S.cur.cut && B > 2000000 && B < 60000000 && lastRel;
  if (S.cur.ok && !S.cur.sceneCopied) S.noScene++;
  if (S.cur.ok && S.prev.ok && S.prev.seq + 1 != S.cur.seq) S.notConsecutive++;
  if (S.cur.ok && S.cur.cut) S.cuts++;
  plan->n = snap_smoothcam::PlanFrame(now, lastRel, P, D, warpOk, &S.gridNext, ScPlan::kMax, plan->alpha, plan->warp, plan->due, &S.lateTicks);
  // test aid SNAP_SMOOTH_CAMERA_TEST_ALPHA=<0..1>: every frame is one re-projected pass at that alpha, then the frame itself, both now, whatever
  // the frame time (frame dumps, NVN_FRAME_DUMP_GEN=1, stall the game and would shut the re-projection off)
  static const double testAlpha = [] { const char* e = getenv("SNAP_SMOOTH_CAMERA_TEST_ALPHA"); return e && *e ? std::clamp(atof(e), 0.0, 1.0) : -1.0; }();
  if (testAlpha >= 0) {
    const bool ok = S.cur.ok && S.cur.sceneCopied && S.prev.ok && S.prev.seq + 1 == S.cur.seq && !S.cur.cut;
    plan->n = 2;
    plan->alpha[0] = (float)testAlpha; plan->warp[0] = ok; plan->due[0] = now;
    plan->alpha[1] = 1.0f; plan->warp[1] = false; plan->due[1] = now;
  }
  return true;
}

VkImage ScRecordPass(Host* h, QueueImpl* q, VkCommandBuffer cb, TextureImpl* src, const ScPlan& plan, int pass, ScShown* shown) {
  *shown = ScShown{};
  if (!S.ready) return VK_NULL_HANDLE;
  std::lock_guard<std::mutex> lk(S.mu);
  shown->on = true; shown->pass = pass; shown->passes = plan.n; shown->frame = S.gameFrames;
  shown->alpha = pass < plan.n ? plan.alpha[pass] : 1.0f;
  shown->due = pass < plan.n ? plan.due[pass] : 0;
  const bool warp = S.active && pass < plan.n && plan.warp[pass] && !S.failed && S.cur.ok && S.prev.ok && src->image && src->HW() == S.dw && src->HH() == S.dh;
  if (S.cur.ok) {
    shown->haveCam = true;
    Angles(S.cur.vp, &shown->yaw, &shown->pitch);
    memcpy(shown->eye, S.cur.eye, sizeof shown->eye);
  }
  if (!warp) return VK_NULL_HANDLE;
  double eye[3];
  const M4 vpA = snap_smoothcam::Between(AsCamera(S.prev), AsCamera(S.cur), plan.alpha[pass], eye);
  M4 inv;
  if (!Invert(S.cur.vp, inv)) return VK_NULL_HANDLE;
  const M4 F = Mul(vpA, inv);
  Angles(vpA, &shown->yaw, &shown->pitch);
  memcpy(shown->eye, eye, sizeof eye);
  shown->warped = true;
  // the finished frame (interface included), once per game frame
  VkImageCopy cp{}; cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; cp.extent = {S.dw, S.dh, 1};
  if (!S.presCopied) {
    Bar(cb, S.pres, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, S.pres.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    Bar(cb, S.pres, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    S.presCopied = true;
  }
  // descriptor ring slot, reused only after the present that last used it finished
  const int k = S.setNext; S.setNext = (S.setNext + 1) % kRing;
  if (S.setValue[k]) {
    uint64_t done = 0; vkGetSemaphoreCounterValue(h->device, q->timeline, &done);
    if (done < S.setValue[k]) {
      VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO}; wi.semaphoreCount = 1; wi.pSemaphores = &q->timeline; wi.pValues = &S.setValue[k];
      vkWaitSemaphores(h->device, &wi, UINT64_MAX);
    }
  }
  S.setValue[k] = q->value + 1;   // this present's submit (queueMutex held until it is submitted)
  const int slot = S.cur.slot;
  VkDescriptorImageInfo ii[4] = {{S.linSamp, S.scene[slot].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                 {S.nearSamp, S.pres.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                 {S.nearSamp, S.depth[slot].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                 {VK_NULL_HANDLE, S.out.view, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet wr[4] = {};
  for (int i = 0; i < 4; i++) {
    wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = S.set[k]; wr[i].dstBinding = (uint32_t)i; wr[i].descriptorCount = 1; wr[i].pImageInfo = &ii[i];
    wr[i].descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  }
  vkUpdateDescriptorSets(h->device, 4, wr, 0, nullptr);
  Bar(cb, S.out, VK_IMAGE_LAYOUT_GENERAL);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.pipeWarp);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.plWarp, 0, 1, &S.set[k], 0, nullptr);
  struct { float r[4][4]; float size[2]; float dsize[2]; float mode[4]; } pc{};
  for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) pc.r[i][j] = (float)F.m[i][j];
  pc.size[0] = (float)S.dw; pc.size[1] = (float)S.dh; pc.dsize[0] = (float)S.rw; pc.dsize[1] = (float)S.rh;
  pc.mode[0] = 0.0f; pc.mode[1] = 0.6f / 255.0f;
  vkCmdPushConstants(cb, S.plWarp, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
  vkCmdDispatch(cb, (S.dw + 7) / 8, (S.dh + 7) / 8, 1);
  Bar(cb, S.out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  Bar(cb, S.show, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  vkCmdCopyImage(cb, S.out.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, S.show.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
  Bar(cb, S.show, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  return S.show.img;
}

void ScNotePresent(const ScShown& s) {
  if (!s.on) return;
  const int64_t now = NowNs();
  S.presents++; S.winPresents++;
  if (s.warped) { S.warped++; S.winWarped++; }
  {
    std::lock_guard<std::mutex> lk(g_traceMu);
    if (g_trace) {
      if (!g_traceT0) g_traceT0 = now;
      if (s.haveCam)
        fprintf(g_trace, "%.3f,%llu,%d,%d,%.4f,%d,%d,%.5f,%.5f,%.4f,%.4f,%.4f,%.3f\n", (double)(now - g_traceT0) * 1e-6, (unsigned long long)s.frame, s.pass, s.passes, s.alpha,
                s.warped ? 1 : 0, S.active ? 1 : 0, s.yaw, s.pitch, s.eye[0], s.eye[1], s.eye[2], s.due ? (double)(now - s.due) * 1e-6 : 0.0);
      else
        fprintf(g_trace, "%.3f,%llu,%d,%d,%.4f,0,%d,,,,,,%.3f\n", (double)(now - g_traceT0) * 1e-6, (unsigned long long)s.frame, s.pass, s.passes, s.alpha, S.active ? 1 : 0,
                s.due ? (double)(now - s.due) * 1e-6 : 0.0);
    }
  }
  if (LogOn()) {
    if (!S.winNs) S.winNs = now;
    if (now - S.winNs >= 1000000000LL) {
      const double sec = (double)(now - S.winNs) * 1e-9;
      fprintf(stderr, "[smoothcam] p%d game %.1f fps, presented %.1f fps, re-projected %.1f fps, active %d (totals: prepared %llu, no scene copy %llu, "
              "not consecutive %llu, cuts %llu; tonemap draws %llu, not pending %llu, size %llu)\n", g_presentCount, S.winFrames / sec, S.winPresents / sec,
              S.winWarped / sec, (int)S.active, (unsigned long long)S.prepared, (unsigned long long)S.noScene, (unsigned long long)S.notConsecutive,
              (unsigned long long)S.cuts, (unsigned long long)S.tmDraws, (unsigned long long)S.tmNotPending, (unsigned long long)S.tmSize);
      S.winPresents = S.winWarped = S.winFrames = 0; S.winNs = now;
    }
  }
}
}  // namespace nvn
