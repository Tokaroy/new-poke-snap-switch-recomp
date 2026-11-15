// SPDX-License-Identifier: MIT
// AMD FSR frame generation, GPU side. See nvn_framegen.h for the data flow; upscale/framegen.cpp is the FidelityFX wrapper.
//
// Images (all device-local, display size, created lazily at the first prepared frame, rebuilt when the window texture changes):
//   hud[4]  UNORM ring, raw copies of the sRGB window texture right after the tonemap/AA draw (before the game draws its UI)
//   pres    UNORM copy of the finished window texture at present (the DLL's presentColor)
//   out     UNORM storage image the DLL writes the interpolated frame into
//   show    sRGB image, a raw copy of `out`, the thing the present blits (a blit from the UNORM image would encode sRGB a second time)
// Raw copies between the sRGB and UNORM twins keep the stored (gamma) bytes unchanged, so no colour conversion happens anywhere.
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "nvn_framegen.h"
#include "nvn_aa.h"
#include "nvn_fx.h"
#include "nvn_internal.h"
#include "nvn_pace.h"
#include "upscale/framegen.h"

extern "C" { void os_host_block_begin(void); void os_host_block_end(void); }

namespace nvn {
namespace {
using snap_upscale::Logf;

struct Img { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkImageLayout lay = VK_IMAGE_LAYOUT_UNDEFINED; };
struct Pend { uint64_t id = 0; int hud = -1; bool reset = false; };

struct State {
  std::mutex mu;
  Host* h = nullptr;
  bool ready = false, failed = false, active = false, hdr = false;
  uint32_t dw = 0, dh = 0;
  VkFormat unorm = VK_FORMAT_UNDEFINED, srgb = VK_FORMAT_UNDEFINED;
  Img hud[4]; int ringNext = 0;
  Img pres, out, show;
  std::unordered_map<const TextureImpl*, Pend> pend;
  bool prevPrepared = false;
  int64_t lastPrepNs = 0;
  // present side
  int64_t lastNs = 0, releaseNs = 0; double dtEma = 0.0; uint64_t sinceNs = 0;
  Pend cur; bool curShow = false;
  // stats
  uint64_t presents = 0, generatedShown = 0, prepared = 0, dispatched = 0, windowPresents = 0, windowGen = 0; int64_t windowNs = 0;
  FILE* trace = nullptr; int64_t traceT0 = 0;
} G;

int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

VkFormat Unorm(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM: return f;
    default: return VK_FORMAT_UNDEFINED;
  }
}
VkFormat Srgb(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
    default: return f;
  }
}

void ImgBar(VkCommandBuffer cb, Img& i, VkImageLayout to) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = i.lay; b.newLayout = to; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = i.img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
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
void FreeAll(Host* h) {
  for (Img& i : G.hud) FreeImg(h, i);
  FreeImg(h, G.pres); FreeImg(h, G.out); FreeImg(h, G.show);
  G.dw = G.dh = 0;
}

// Caller holds G.mu.
bool EnsureImages(uint32_t w, uint32_t hh, VkFormat unorm) {
  if (G.dw == w && G.dh == hh && G.unorm == unorm && G.out.img) return true;
  Host* h = G.h;
  if (G.dw) vkDeviceWaitIdle(h->device);   // a window resize: rare, and nothing recorded may still read the old images
  FreeAll(h);
  G.pend.clear(); G.prevPrepared = false;
  const VkImageUsageFlags colorIn = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  bool ok = true;
  for (Img& i : G.hud) ok = ok && MakeImg(h, i, w, hh, unorm, colorIn);
  ok = ok && MakeImg(h, G.pres, w, hh, unorm, colorIn);
  ok = ok && MakeImg(h, G.out, w, hh, unorm, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  ok = ok && MakeImg(h, G.show, w, hh, Srgb(unorm), VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  if (!ok) { FreeAll(h); G.failed = true; Logf("framegen: image creation failed (%ux%u): frame generation off for this run", w, hh); return false; }
  G.dw = w; G.dh = hh; G.unorm = unorm; G.srgb = Srgb(unorm);
  Logf("framegen: images %ux%u created", w, hh);
  return true;
}

void Fail(const char* why) { G.failed = true; G.active = false; Logf("framegen: %s: frame generation off for this run", why); }

void Norm(float* v) {
  const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (l > 1e-8f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}
void Cross(const float* a, const float* b, float* o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }

void WaitUntilNs(int64_t t) {
  static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  const int64_t rem = t - NowNs();
  if (rem > 600000 && timer) {
    LARGE_INTEGER due; due.QuadPart = -(rem - 400000) / 100;   // wake 0.4 ms early, spin the rest
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
  } else if (rem > 1500000) Sleep((DWORD)((rem - 1000000) / 1000000));
  while (NowNs() < t) YieldProcessor();
}

double PanelHz() {
  static const double env = [] { const char* e = getenv("SNAP_FRAMEGEN_PANEL"); return e ? atof(e) : 0.0; }();
  return env > 0 ? env : snap_pace::PanelHz();
}
bool ForceOn() { static const bool f = [] { const char* e = getenv("SNAP_FRAMEGEN_FORCE"); return e && e[0] == '1'; }(); return f; }
bool LogOn() { static const bool f = [] { const char* e = getenv("SNAP_FRAMEGEN_LOG"); return e && e[0] == '1'; }(); return f; }
}  // namespace

bool FgWanted() { return snap_framegen::Wanted(); }

void FgDeviceCreated(Host* h) {
  if (!snap_framegen::Wanted()) return;
  G.h = h;
  if (!snap_framegen::Load()) { G.failed = true; return; }
  snap_upscale::DeviceCtx c;
  c.instance = h->instance; c.phys = h->phys; c.device = h->device; c.queue = h->queue; c.family = h->family;
  if (!snap_framegen::Init(c)) { G.failed = true; Logf("framegen: initialization failed: frame generation unavailable"); return; }
  G.ready = true;
  if (const char* t = getenv("SNAP_FRAMEGEN_TRACE")) if (*t) { G.trace = fopen(t, "w"); if (G.trace) fprintf(G.trace, "t_ms,generated,active\n"); }
  Logf("framegen: ready (panel %.1f Hz%s)", PanelHz(), ForceOn() ? ", rate gate forced open" : "");
}

void FgDestroy(Host* h) {
  if (!G.h || G.h != h) return;
  if (G.presents) Logf("framegen: summary: %llu presents, %llu generated frames shown, %llu frames prepared, %llu dispatched, %u failures",
                       (unsigned long long)G.presents, (unsigned long long)G.generatedShown, (unsigned long long)G.prepared, (unsigned long long)G.dispatched, snap_framegen::Failures());
  snap_framegen::Release();
  { std::lock_guard<std::mutex> lk(G.mu); vkDeviceWaitIdle(h->device); FreeAll(h); G.pend.clear(); }
  if (G.trace) { fclose(G.trace); G.trace = nullptr; }
  G.ready = false; G.h = nullptr;
}

void FgForgetTexture(const TextureImpl* t) {
  if (!G.ready) return;
  std::lock_guard<std::mutex> lk(G.mu);
  G.pend.erase(t);
}

bool FgActive() { return G.ready && !G.failed && G.active; }

bool FgPrepare(ReplayCtx& cx, const FgScene& s) {
  if (!G.ready || G.failed || !snap_framegen::Usable()) return false;
  std::lock_guard<std::mutex> lk(G.mu);
  if (!G.active || G.hdr || !s.display || !s.display->fi || !s.display->image) { G.prevPrepared = false; return false; }
  const VkFormat un = Unorm(s.display->fi->vk);
  if (un == VK_FORMAT_UNDEFINED) { Fail("window texture format is not RGBA8"); return false; }
  const uint32_t dw = s.display->HW(), dh = s.display->HH();
  if (!EnsureImages(dw, dh, un)) return false;
  if (!snap_framegen::EnsureContext(dw, dh, std::max(s.rw, 1u), std::max(s.rh, 1u), un)) { G.prevPrepared = false; if (snap_framegen::Failures() > 3) G.failed = true; return false; }
  const int64_t now = NowNs();
  snap_framegen::PrepareIn in;
  in.cb = cx.cb;
  in.depth = {s.depth, s.depthView, s.depthFormat};
  in.mv = {s.mv, s.mvView, VK_FORMAT_R16G16_SFLOAT};
  in.w = s.rw; in.h = s.rh; in.jx = s.jx; in.jy = s.jy;
  const double dt = G.lastPrepNs ? (double)(now - G.lastPrepNs) * 1e-6 : 0.0;
  in.dtMs = dt > 1.0 && dt < 100.0 ? (float)dt : 16.67f;
  G.lastPrepNs = now;
  in.nearZ = s.nearZ; in.farZ = s.farZ; in.fovY = s.fovY;
  in.reset = s.cut || !G.prevPrepared;
  memcpy(in.pos, s.eye, sizeof in.pos);
  memcpy(in.fwd, s.fwd, sizeof in.fwd); Norm(in.fwd);
  const float worldUp[3] = {0, 1, 0};
  Cross(in.fwd, worldUp, in.right);
  if (in.right[0] * in.right[0] + in.right[1] * in.right[1] + in.right[2] * in.right[2] < 1e-8f) { in.right[0] = 1; in.right[1] = 0; in.right[2] = 0; }   // looking straight up or down
  Norm(in.right);
  Cross(in.right, in.fwd, in.up); Norm(in.up);
  in.depthIsDsReadOnly = true;
  const uint64_t id = snap_framegen::Prepare(in);
  if (!id) { G.prevPrepared = false; if (snap_framegen::Failures() > 3) G.failed = true; return false; }
  Pend p; p.id = id; p.reset = in.reset;
  G.pend[s.display] = p;
  G.prevPrepared = true; G.prepared++;
  return true;
}

void FgAfterDraw(ReplayCtx& cx, const char* key) {
  if (!G.ready) return;
  TextureImpl* t = cx.rt[0];
  if (!t || !t->image || !t->fi || !AaIsTonemapKey(key)) return;
  std::lock_guard<std::mutex> lk(G.mu);
  auto it = G.pend.find(t);
  if (it == G.pend.end() || G.failed) return;
  EndRendering(cx);
  SubRange rr;
  t->RenderView(cx.rtHasView[0] ? &cx.rtView[0] : nullptr, &rr);
  const uint32_t w = t->LevelW(rr.lb), hh = t->LevelH(rr.lb);
  if (rr.yn != 1 || w != G.dw || hh != G.dh) return;
  if (it->second.hud < 0) it->second.hud = G.ringNext++ & 3;   // a second tonemap-keyed draw of the same frame overwrites the same slot
  Img& hud = G.hud[it->second.hud];
  Host* h = cx.h; VkCommandBuffer cb = cx.cb;
  TransitionRange(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rr);
  ImgBar(cb, hud, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkImageCopy cp{}; cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, rr.lb, rr.yb, 1}; cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; cp.extent = {w, hh, 1};
  vkCmdCopyImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hud.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
  ImgBar(cb, hud, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  TransitionRange(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, rr);
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cb, &di);
  // only copies were recorded (no viewport or scissor was touched), so the next draw re-begins rendering with the game's state as it was
}

bool FgBeginPresent(Host*, TextureImpl* src, bool hdr) {
  if (!G.ready) return false;
  std::lock_guard<std::mutex> lk(G.mu);
  const int64_t now = NowNs();
  if (G.lastNs) {
    const double d = (double)(now - G.lastNs) * 1e-9;
    if (d > 0.002 && d < 0.25) G.dtEma = G.dtEma > 0 ? G.dtEma + (d - G.dtEma) * 0.1 : d;
  }
  G.lastNs = now; G.releaseNs = now; G.hdr = hdr;
  if (G.failed) { G.active = false; G.pend.erase(src); return false; }
  const bool was = G.active;
  bool want = G.dtEma > 0 && snap_framegen::RateGate(1.0 / G.dtEma, PanelHz(), was, (uint64_t)now, &G.sinceNs);
  if (ForceOn() && G.dtEma > 0) want = true;
  if (hdr) want = false;
  G.active = want;
  if (G.active != was) Logf("framegen: %s (base %.1f fps, panel %.1f Hz)", want ? "on" : "off", G.dtEma > 0 ? 1.0 / G.dtEma : 0.0, PanelHz());
  auto it = G.pend.find(src);
  if (it == G.pend.end()) { G.prevPrepared = false; return false; }   // a real-only frame (photo guard, menu, hold): the next prepared frame starts a new history
  G.cur = it->second; G.pend.erase(it);
  G.curShow = G.active && !G.cur.reset && G.cur.hud >= 0 && snap_framegen::Usable();
  return true;
}

VkImage FgRecordGenerate(Host*, VkCommandBuffer cb, TextureImpl* src) {
  std::lock_guard<std::mutex> lk(G.mu);
  if (G.failed || !G.out.img || src->HW() != G.dw || src->HH() != G.dh || !src->image) return VK_NULL_HANDLE;
  // the present colour: a raw copy of the finished window texture (HUD and menus included)
  ImgBar(cb, G.pres, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkImageCopy cp{}; cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; cp.extent = {G.dw, G.dh, 1};
  vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, G.pres.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
  ImgBar(cb, G.pres, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  ImgBar(cb, G.out, VK_IMAGE_LAYOUT_GENERAL);
  snap_framegen::GenerateIn gi;
  gi.cb = cb; gi.frameId = G.cur.id;
  if (G.cur.hud >= 0) { const Img& hu = G.hud[G.cur.hud]; gi.hudless = {hu.img, hu.view, G.unorm}; }
  gi.present = {G.pres.img, G.pres.view, G.unorm};
  gi.out = {G.out.img, G.out.view, G.unorm};
  gi.w = G.dw; gi.h = G.dh; gi.reset = G.cur.reset;
  const bool ok = snap_framegen::Generate(gi);
  G.dispatched++;
  // the DLL leaves what it was told: `pres` and the hud image back in SHADER_READ_ONLY, `out` in GENERAL
  if (!ok) { if (snap_framegen::Failures() > 3) G.failed = true; return VK_NULL_HANDLE; }
  if (!G.curShow) return VK_NULL_HANDLE;
  ImgBar(cb, G.out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  ImgBar(cb, G.show, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  vkCmdCopyImage(cb, G.out.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, G.show.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
  ImgBar(cb, G.show, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  return G.show.img;
}

void FgSleepHalf() {
  const double t = std::clamp(G.dtEma > 0 ? G.dtEma : 0.0167, 0.008, 0.05);
  os_host_block_begin();
  WaitUntilNs(G.releaseNs + (int64_t)(t * 0.5e9));
  os_host_block_end();
}

void FgNotePresent(bool generated) {
  if (!G.ready) return;
  const int64_t now = NowNs();
  G.presents++; G.windowPresents++;
  if (generated) { G.generatedShown++; G.windowGen++; }
  if (G.trace) {
    if (!G.traceT0) G.traceT0 = now;
    fprintf(G.trace, "%.3f,%d,%d\n", (double)(now - G.traceT0) * 1e-6, generated ? 1 : 0, G.active ? 1 : 0);
  }
  if (LogOn()) {
    if (!G.windowNs) G.windowNs = now;
    if (now - G.windowNs >= 1000000000LL) {
      const double s = (double)(now - G.windowNs) * 1e-9;
      Logf("framegen: p%d base %.1f fps, presented %.1f fps, generated %.1f fps, active %d, failures %u", g_presentCount, (G.windowPresents - G.windowGen) / s,
           G.windowPresents / s, G.windowGen / s, (int)G.active, snap_framegen::Failures());
      G.windowPresents = G.windowGen = 0; G.windowNs = now;
    }
  }
}
}  // namespace nvn
