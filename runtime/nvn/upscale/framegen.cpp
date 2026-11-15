// SPDX-License-Identifier: MIT
// AMD FSR frame generation (FidelityFX SDK 2.4.0 API, frame generation 4.0.2). See framegen.h.
// Flow, as the DLL wants it with no swapchain wrapper (flag NO_SWAPCHAIN_CONTEXT_NOTIFY: interpolate only, the host presents):
//   per rendered frame, on the scene command buffer:  Prepare (depth, motion vectors, camera, frame id N)
//   per present, on the present command buffer:        Configure (HUD-less colour, frame id N) + Dispatch (finished frame -> output)
// The DLL fails closed: any error counts a failure and more than 3 turn frame generation off for the run.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <cstring>

#include "framegen.h"

#if defined(SNAP_HAVE_FFX_API)
#include <ffx_api.h>
#include <ffx_framegeneration.h>
#include <vk/ffx_api_framegeneration_vk.h>
#include <vk/ffx_api_vk.h>

namespace snap_framegen {
namespace {
using snap_upscale::Logf;

struct State {
  HMODULE lib = nullptr;
  PfnFfxCreateContext create = nullptr; PfnFfxDestroyContext destroy = nullptr; PfnFfxDispatch dispatch = nullptr; PfnFfxConfigure configure = nullptr;
  bool tried = false, loaded = false;
  DeviceCtx dc;
  ffxContext ctx = nullptr;
  uint32_t dw = 0, dh = 0, rw = 0, rh = 0; VkFormat fmt = VK_FORMAT_UNDEFINED;
  uint64_t nextId = 1;        // the next prepare uses nextId, then increments (ids go up by exactly one per prepared frame)
  uint32_t fails = 0;
  uint64_t generated = 0;
  bool wanted = false, wantedRead = false;
} F;

void OnMessage(uint32_t type, const wchar_t* message) { Logf("framegen: %s: %ls", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", message); }

FfxApiResource Res(const Image& m, uint32_t w, uint32_t h, uint32_t usage, uint32_t state) {
  FfxApiResourceDescription d{};
  d.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
  d.format = ffxApiGetSurfaceFormatVK(m.format);
  d.width = w; d.height = h; d.depth = 1; d.mipCount = 1;
  d.flags = FFX_API_RESOURCE_FLAGS_NONE;
  d.usage = usage;
  return ffxApiGetResourceVK((void*)m.image, d, state);
}

// Same device-function shim as upscale_fsr.cpp: the prebuilt backend calls some functions without a null check.
VKAPI_ATTR void VKAPI_CALL NoLabel(VkCommandBuffer, const VkDebugUtilsLabelEXT*) {}
VKAPI_ATTR void VKAPI_CALL NoEndLabel(VkCommandBuffer) {}
VKAPI_ATTR VkResult VKAPI_CALL NoName(VkDevice, const VkDebugUtilsObjectNameInfoEXT*) { return VK_SUCCESS; }
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ProcAddr(VkDevice dev, const char* name) {
  PFN_vkVoidFunction f = vkGetDeviceProcAddr(dev, name);
  if (f) return f;
  const size_t n = strlen(name);
  if (n > 3 && (!strcmp(name + n - 3, "KHR") || !strcmp(name + n - 3, "EXT"))) {
    char core[128] = {};
    if (n - 3 < sizeof core) { memcpy(core, name, n - 3); f = vkGetDeviceProcAddr(dev, core); }
    if (f) return f;
  }
  if (!strcmp(name, "vkCmdBeginDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)NoLabel;
  if (!strcmp(name, "vkCmdEndDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)NoEndLabel;
  if (!strcmp(name, "vkSetDebugUtilsObjectNameEXT")) return (PFN_vkVoidFunction)NoName;
  Logf("framegen: device function %s not available", name);
  return nullptr;
}

void Fail(const char* what, ffxReturnCode_t r) {
  F.fails++;
  Logf("framegen: %s failed (%u)%s", what, (unsigned)r, F.fails > 3 ? ": frame generation off for this run" : "");
}

void DepthLayout(VkCommandBuffer cb, const Image& depth, VkImageLayout from, VkImageLayout to) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = from; b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = depth.image;
  const bool st = depth.format == VK_FORMAT_D24_UNORM_S8_UINT || depth.format == VK_FORMAT_D32_SFLOAT_S8_UINT || depth.format == VK_FORMAT_D16_UNORM_S8_UINT;
  b.subresourceRange = {VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | (st ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)), 0, 1, 0, 1};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cb, &di);
}
}  // namespace

bool Wanted() {
  if (!F.wantedRead) {
    F.wantedRead = true;
    const char* e = getenv("SNAP_FRAMEGEN");
    F.wanted = e && e[0] == '1';
  }
  return F.wanted;
}

bool Load() {
  if (F.tried) return F.loaded;
  F.tried = true;
  wchar_t wpath[MAX_PATH + 64] = {};
  DWORD len = GetModuleFileNameW(nullptr, wpath, MAX_PATH);
  while (len && wpath[len - 1] != L'\\') len--;
  wcscpy(wpath + len, L"upscalers\\amd_fidelityfx_framegeneration_vk.dll");
  char path[MAX_PATH * 3] = {};
  WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, (int)sizeof path, nullptr, nullptr);
  if (GetFileAttributesW(wpath) == INVALID_FILE_ATTRIBUTES) { Logf("framegen: %s is not installed: frame generation unavailable (fetch it with the launcher or the builder)", path); return false; }
  F.lib = LoadLibraryExW(wpath, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);   // absolute path only
  if (!F.lib) { Logf("framegen: cannot load %s (error %lu): frame generation unavailable", path, GetLastError()); return false; }
  F.create = (PfnFfxCreateContext)(void*)GetProcAddress(F.lib, "ffxCreateContext");
  F.destroy = (PfnFfxDestroyContext)(void*)GetProcAddress(F.lib, "ffxDestroyContext");
  F.dispatch = (PfnFfxDispatch)(void*)GetProcAddress(F.lib, "ffxDispatch");
  F.configure = (PfnFfxConfigure)(void*)GetProcAddress(F.lib, "ffxConfigure");
  if (!F.create || !F.destroy || !F.dispatch || !F.configure) { Logf("framegen: %s lacks the FidelityFX API entry points: frame generation unavailable", path); return false; }
  Logf("framegen: %s (FidelityFX SDK 2.4.0 API, frame generation 4.0.2)", path);
  return F.loaded = true;
}

bool Init(const DeviceCtx& c) { if (!F.loaded) return false; F.dc = c; return true; }
bool Usable() { return F.loaded && F.fails <= 3; }
uint64_t FramesGenerated() { return F.generated; }
uint32_t Failures() { return F.fails; }

static void DestroyCtx() {
  if (!F.ctx) return;
  vkDeviceWaitIdle(F.dc.device);
  F.destroy(&F.ctx, nullptr);
  F.ctx = nullptr;
}

void Release() { DestroyCtx(); }

bool EnsureContext(uint32_t dw, uint32_t dh, uint32_t rw, uint32_t rh, VkFormat fmt) {
  if (!Usable()) return false;
  if (F.ctx && F.dw == dw && F.dh == dh && F.fmt == fmt && rw <= F.rw && rh <= F.rh) return true;
  DestroyCtx();
  ffxCreateBackendVKDesc be{};
  be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
  be.deviceContext.vkDevice = F.dc.device; be.deviceContext.vkPhysicalDevice = F.dc.phys; be.deviceContext.vkDeviceProcAddr = ProcAddr;
  ffxCreateContextDescFrameGenerationVersion ver{};
  ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
  ver.version = FFX_FRAMEGENERATION_VERSION;
  ver.header.pNext = &be.header;
  ffxCreateContextDescFrameGeneration cd{};
  cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
  cd.header.pNext = &ver.header;
  cd.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;   // reversed infinite Z
  cd.displaySize = {dw, dh};
  cd.maxRenderSize = {rw, rh};
  cd.backBufferFormat = ffxApiGetSurfaceFormatVK(fmt);
  LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&fq);
  ffxReturnCode_t r = F.create(&F.ctx, &cd.header, nullptr);
  if (r != FFX_API_RETURN_OK || !F.ctx) { F.ctx = nullptr; Fail("context creation", r); return false; }
  ffxConfigureDescFrameGeneration cf{};
  cf.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
  cf.frameGenerationEnabled = true;
  cf.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
  cf.generationRect = {0, 0, (int32_t)dw, (int32_t)dh};
  cf.frameID = F.nextId;
  r = F.configure(&F.ctx, &cf.header);
  if (r != FFX_API_RETURN_OK) { F.destroy(&F.ctx, nullptr); F.ctx = nullptr; Fail("configure", r); return false; }
  F.dw = dw; F.dh = dh; F.rw = rw; F.rh = rh; F.fmt = fmt;
  QueryPerformanceCounter(&t1);
  Logf("framegen: FSR frame generation context %ux%u (render up to %ux%u), created in %.1f ms", dw, dh, rw, rh, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart);
  return true;
}

uint64_t Prepare(const PrepareIn& in) {
  if (!F.ctx || !Usable()) return 0;
  ffxDispatchDescFrameGenerationPrepareV2 d{};
  d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
  const uint64_t id = F.nextId;
  d.frameID = id;
  d.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
  d.commandList = (void*)in.cb;
  d.renderSize = {in.w, in.h};
  d.jitterOffset = {in.jx, in.jy};
  d.motionVectorScale = {1.0f, 1.0f};   // vectors are in render pixels
  d.frameTimeDelta = in.dtMs;
  d.reset = in.reset;
  d.cameraNear = in.nearZ; d.cameraFar = in.farZ; d.cameraFovAngleVertical = in.fovY;
  d.viewSpaceToMetersFactor = 1.0f;
  d.depth = Res(in.depth, in.w, in.h, FFX_API_RESOURCE_USAGE_DEPTHTARGET, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.motionVectors = Res(in.mv, in.w, in.h, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  memcpy(d.cameraPosition, in.pos, sizeof d.cameraPosition);
  memcpy(d.cameraUp, in.up, sizeof d.cameraUp);
  memcpy(d.cameraRight, in.right, sizeof d.cameraRight);
  memcpy(d.cameraForward, in.fwd, sizeof d.cameraForward);
  if (in.depthIsDsReadOnly) DepthLayout(in.cb, in.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  ffxReturnCode_t r = F.dispatch(&F.ctx, &d.header);
  if (in.depthIsDsReadOnly) DepthLayout(in.cb, in.depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
  if (r != FFX_API_RETURN_OK) { Fail("prepare", r); return 0; }
  F.nextId++;
  return id;
}

bool Generate(const GenerateIn& in) {
  if (!F.ctx || !Usable() || !in.frameId) return false;
  ffxConfigureDescFrameGeneration cf{};
  cf.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
  cf.frameGenerationEnabled = true;
  cf.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
  cf.generationRect = {0, 0, (int32_t)in.w, (int32_t)in.h};
  cf.frameID = in.frameId;
  if (in.hudless.image) cf.HUDLessColor = Res(in.hudless, in.w, in.h, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  ffxReturnCode_t r = F.configure(&F.ctx, &cf.header);
  if (r != FFX_API_RETURN_OK) { Fail("configure", r); return false; }
  ffxDispatchDescFrameGeneration d{};
  d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
  d.commandList = (void*)in.cb;
  d.presentColor = Res(in.present, in.w, in.h, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.outputs[0] = Res(in.out, in.w, in.h, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  d.numGeneratedFrames = 1;
  d.reset = in.reset;
  d.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;   // Snap's SDR picture; the images are described as UNORM
  d.minMaxLuminance[0] = 0.0f; d.minMaxLuminance[1] = 1.0f;
  d.generationRect = {0, 0, (int32_t)in.w, (int32_t)in.h};
  d.frameID = in.frameId;
  r = F.dispatch(&F.ctx, &d.header);
  if (r != FFX_API_RETURN_OK) { Fail("dispatch", r); return false; }
  F.generated++;
  return true;
}

}  // namespace snap_framegen

#else   // no FidelityFX API headers at build time

namespace snap_framegen {
bool Wanted() { return false; }
bool Load() { snap_upscale::Logf("framegen: built without the FidelityFX API headers (tools\\amd\\): frame generation unavailable"); return false; }
bool Init(const DeviceCtx&) { return false; }
bool Usable() { return false; }
void Release() {}
bool EnsureContext(uint32_t, uint32_t, uint32_t, uint32_t, VkFormat) { return false; }
uint64_t Prepare(const PrepareIn&) { return 0; }
bool Generate(const GenerateIn&) { return false; }
uint64_t FramesGenerated() { return 0; }
uint32_t Failures() { return 0; }
}  // namespace snap_framegen

#endif

namespace snap_framegen {
// Frame generation runs while the base rate is at least about 50 fps (switches on at 52 and off below 48, so it does not flicker
// at the limit) and, when the panel rate is known, the doubled rate fits it (base at most 55% of the panel). A change of state
// needs 2 s of disagreement.
bool RateGate(double baseHz, double panelHz, bool current, uint64_t nowNs, uint64_t* sinceNs) {
  const double lo = current ? 48.0 : 52.0;
  const bool want = baseHz >= lo && (panelHz <= 0 || baseHz <= 0.55 * panelHz + (current ? 4.0 : 0.0));
  if (want == current) { *sinceNs = 0; return current; }
  if (!*sinceNs) *sinceNs = nowNs ? nowNs : 1;
  if (nowNs < *sinceNs + 2000000000ull) return current;
  *sinceNs = 0;
  return want;
}
}  // namespace snap_framegen
