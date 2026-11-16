// SPDX-License-Identifier: MIT
// Free camera clean shot. See nvn_freecam.h.
#include "nvn_freecam.h"

#include <windows.h>
#include <wincodec.h>
#include <objidl.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "hle/hle_config.h"
#include "nvn_aa.h"
#include "nvn_internal.h"

namespace nvn {
namespace {
std::atomic<int> g_req{0};
struct Pending { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void* map = nullptr; uint32_t w = 0, h = 0; bool bgra = false; int present = 0; };
Pending g_p;   // render thread only (FcShotAfterDraw and FcShotAfterSubmit both run on the present path)
int g_n;

std::wstring widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
  return w;
}

bool write_png(const std::string& path, const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h) {
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // RPC_E_CHANGED_MODE: the thread is already in an apartment, which is fine
  IWICImagingFactory* f = nullptr;
  bool ok = false;
  if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
    IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* pb = nullptr;
    const std::wstring tmp = widen(path + ".tmp");
    if (SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, &pb)) && SUCCEEDED(fr->Initialize(pb)) && SUCCEEDED(fr->SetSize(w, h))) {
      WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
      fr->SetPixelFormat(&pf);
      if (SUCCEEDED(fr->WritePixels(h, w * 4, (UINT)bgra.size(), (BYTE*)bgra.data())) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit())) ok = true;
    }
    if (pb) pb->Release();
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    f->Release();
    if (ok) ok = MoveFileExW(tmp.c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    else DeleteFileW(tmp.c_str());
  }
  if (SUCCEEDED(co)) CoUninitialize();
  return ok;
}
}  // namespace

void FcShotAfterDraw(ReplayCtx& cx, const char* key) {
  if (!g_req.load(std::memory_order_relaxed) || g_p.buf) return;
  TextureImpl* t = cx.rt[0];
  if (!t || !t->image || !t->fi || t->fi->bytes != 4 || t->fi->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !AaIsTonemapKey(key)) return;
  EndRendering(cx);
  SubRange rr;
  t->RenderView(cx.rtHasView[0] ? &cx.rtView[0] : nullptr, &rr);
  if (rr.yn != 1) return;
  const uint32_t w = t->LevelW(rr.lb), hh = t->LevelH(rr.lb);
  if (!w || !hh) return;
  g_req.store(0);
  Pending p;
  if (!CreateHostBuffer(cx.h, (uint64_t)w * hh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &p.buf, &p.mem, &p.map)) { fprintf(stderr, "[freecam] shot: buffer allocation failed\n"); return; }
  p.w = w; p.h = hh; p.present = g_presentCount;
  p.bgra = t->fi->vk == VK_FORMAT_B8G8R8A8_UNORM || t->fi->vk == VK_FORMAT_B8G8R8A8_SRGB;
  VkCommandBuffer cb = cx.cb;
  TransitionRange(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rr);
  VkBufferImageCopy bc{}; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, rr.lb, rr.yb, 1}; bc.imageExtent = {w, hh, 1};
  vkCmdCopyImageToBuffer(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.buf, 1, &bc);
  TransitionRange(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, rr);
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT; mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cb, &di);
  g_p = p;   // only copies were recorded: the next draw re-begins rendering with the game's state as it was
}

void FcShotAfterSubmit(Host* h, VkSemaphore timeline, uint64_t value) {
  if (!g_p.buf) return;
  const Pending p = g_p;
  g_p = Pending{};
  VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
  wi.semaphoreCount = 1; wi.pSemaphores = &timeline; wi.pValues = &value;
  vkWaitSemaphores(h->device, &wi, UINT64_MAX);
  std::vector<uint8_t> px((size_t)p.w * p.h * 4);
  const uint8_t* in = static_cast<const uint8_t*>(p.map);
  for (size_t i = 0, n = (size_t)p.w * p.h; i < n; i++) {
    const uint8_t* s = in + i * 4; uint8_t* d = px.data() + i * 4;
    d[0] = p.bgra ? s[0] : s[2]; d[1] = s[1]; d[2] = p.bgra ? s[2] : s[0]; d[3] = 255;
  }
  vkDestroyBuffer(h->device, p.buf, nullptr);
  vkFreeMemory(h->device, p.mem, nullptr);
  std::string dir = hle::config().screenshot_dir.empty() ? hle::config().data_root + "/screenshots" : hle::config().screenshot_dir;
  dir += "/freecam";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  SYSTEMTIME st; GetLocalTime(&st);
  char name[160];
  snprintf(name, sizeof name, "/freecam_%04d%02d%02d_%02d%02d%02d_%d.png", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ++g_n);
  const std::string path = dir + name;
  const bool ok = write_png(path, px, p.w, p.h);
  fprintf(stderr, "[freecam] clean shot %s %s (%ux%u, present %d)\n", ok ? "saved" : "FAILED", path.c_str(), p.w, p.h, p.present);
}
}  // namespace nvn

extern "C" void nvnHostFreecamShot(void) { nvn::g_req.store(1); }
