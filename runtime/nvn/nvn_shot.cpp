// SPDX-License-Identifier: MIT
// Screenshot key. See nvn_shot.h.
#include "nvn_shot.h"

#include <windows.h>
#include <objidl.h>
#include <shlobj.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include "hle/input_core.h"
#include "hle/settings.h"
#include "nvn_aa.h"
#include "nvn_internal.h"

#pragma comment(lib, "windowscodecs")

namespace nvn {
namespace {
constexpr int kFallbackPresents = 20;   // no tonemap draw this long (menus, loading): take the window texture with the HUD

struct Copy { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void* map = nullptr; uint32_t w = 0, h = 0; bool bgra = false, hud = false; int present = 0; };
std::mutex g_mu;           // the draw replay and the present can run on different threads
std::atomic<bool> g_want{false};   // a request is waiting for its copy (ShotAfterDraw peeks without the lock)
bool g_hud = false;        // that request includes the HUD
int g_armedAt = 0;         // present count when it was taken
Copy g_rec;                // recorded, not yet handed to the writer
Copy g_shown;              // with a HUD-less g_rec: the window texture of the same frame, saved instead if g_rec is blank
constexpr int kMaxInFlight = 3;   // shots handed to the writer and not yet written (each holds ~75 MB at 5760x3240, a PNG takes seconds)
std::atomic<int> g_inFlight{0};
std::atomic<int> g_n{0};

std::wstring widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
  return w;
}
std::string narrow(const std::wstring& w) {
  if (w.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

bool hud_wanted() {
  if (const char* e = std::getenv("SNAP_SHOT_HUD"); e && *e) return e[0] == '1';
  return hle::settings::get_bool("shot_hud", false);
}

std::string shot_dir() {
  if (const char* e = std::getenv("SNAP_SHOT_DIR"); e && *e) return e;
  std::string d = hle::settings::get("shot_dir", "");
  if (!d.empty()) return d;
  PWSTR p = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &p)) && p) d = narrow(p) + "\\Snap Recomp";
  if (p) CoTaskMemFree(p);
  if (d.empty()) { const char* u = std::getenv("USERPROFILE"); d = std::string(u ? u : ".") + "\\Pictures\\Snap Recomp"; }
  return d;
}

// The writer thread: BelowNormal, jobs in order.
struct Writer {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::function<void()>> q;
  bool started = false;
};
Writer& writer() { static Writer* w = new Writer; return *w; }   // never destroyed: the thread may outlive static teardown

bool copy_ok(TextureImpl* t) {
  if (!t || !t->image || !t->fi || t->fi->bytes != 4 || t->fi->aspect != VK_IMAGE_ASPECT_COLOR_BIT) return false;
  const VkFormat f = t->fi->vk;
  return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
}

void host_read_barrier(VkCommandBuffer cb) {
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
  mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cb, &di);
}

// Records the copy of one subresource (already in TRANSFER_SRC) into a new host buffer. g_mu held.
bool record_copy(Host* h, VkCommandBuffer cb, TextureImpl* t, uint32_t level, uint32_t layer, uint32_t w, uint32_t hh, bool hud, Copy& out) {
  Copy c;
  if (!CreateHostBuffer(h, (uint64_t)w * hh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &c.buf, &c.mem, &c.map)) {
    fprintf(stderr, "[shot] buffer allocation failed (%ux%u)\n", w, hh);
    return false;
  }
  c.w = w; c.h = hh; c.hud = hud; c.present = g_presentCount;
  c.bgra = t->fi->vk == VK_FORMAT_B8G8R8A8_UNORM || t->fi->vk == VK_FORMAT_B8G8R8A8_SRGB;
  VkBufferImageCopy bc{}; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1}; bc.imageExtent = {w, hh, 1};
  vkCmdCopyImageToBuffer(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.buf, 1, &bc);
  out = c;
  return true;
}

void free_copy(VkDevice dev, Copy& c) {
  if (c.buf) vkDestroyBuffer(dev, c.buf, nullptr);
  if (c.mem) vkFreeMemory(dev, c.mem, nullptr);
  c = Copy{};
}

// A menu frame can still run the tonemap draw over an empty scene (photo select: black). Blank = every sampled pixel
// (a 64x36 grid) under 4 in all channels.
bool blank(const Copy& c) {
  const auto* px = static_cast<const uint8_t*>(c.map);
  for (uint32_t gy = 0; gy < 36; gy++)
    for (uint32_t gx = 0; gx < 64; gx++) {
      const uint8_t* p = px + ((size_t)(gy * c.h / 36) * c.w + gx * c.w / 64) * 4;
      if (p[0] >= 4 || p[1] >= 4 || p[2] >= 4) return false;
    }
  return true;
}
}  // namespace

bool ShotWritePng(const std::string& path, const uint8_t* px, uint32_t w, uint32_t h, bool bgra) {
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // RPC_E_CHANGED_MODE: already in an apartment, which is fine
  IWICImagingFactory* f = nullptr;
  bool ok = false;
  const std::wstring tmp = widen(path + ".tmp");
  if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
    IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* pb = nullptr;
    if (SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, &pb)) && SUCCEEDED(fr->Initialize(pb)) && SUCCEEDED(fr->SetSize(w, h))) {
      WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
      if (SUCCEEDED(fr->SetPixelFormat(&pf)) && IsEqualGUID(pf, GUID_WICPixelFormat24bppBGR)) {
        constexpr uint32_t kStrip = 64;   // rows converted at a time: no second full-size copy
        std::vector<uint8_t> row((size_t)w * 3 * kStrip);
        ok = true;
        for (uint32_t y0 = 0; ok && y0 < h; y0 += kStrip) {
          const uint32_t n = std::min(kStrip, h - y0);
          for (uint32_t y = 0; y < n; y++) {
            const uint8_t* s = px + (size_t)(y0 + y) * w * 4;
            uint8_t* d = row.data() + (size_t)y * w * 3;
            for (uint32_t x = 0; x < w; x++, s += 4, d += 3) { d[0] = bgra ? s[0] : s[2]; d[1] = s[1]; d[2] = bgra ? s[2] : s[0]; }
          }
          ok = SUCCEEDED(fr->WritePixels(n, w * 3, n * w * 3, row.data()));
        }
        ok = ok && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
      }
    }
    if (pb) pb->Release();
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    f->Release();
  }
  if (ok) ok = MoveFileExW(tmp.c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
  if (!ok) DeleteFileW(tmp.c_str());
  if (SUCCEEDED(co)) CoUninitialize();
  return ok;
}

void ShotPost(std::function<void()> job) {
  Writer& W = writer();
  std::lock_guard<std::mutex> lk(W.mu);
  W.q.push_back(std::move(job));
  if (!W.started) {
    W.started = true;
    std::thread([&W] {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
      SetThreadDescription(GetCurrentThread(), L"snap shot writer");
      for (;;) {
        std::function<void()> j;
        {
          std::unique_lock<std::mutex> l(W.mu);
          W.cv.wait(l, [&W] { return !W.q.empty(); });
          j = std::move(W.q.front());
          W.q.pop_front();
        }
        j();
      }
    }).detach();
  }
  W.cv.notify_one();
}

void ShotOnPresent(Host* h, VkCommandBuffer cb, TextureImpl* src) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (hle::input::consume_screenshot()) {
    if (g_inFlight.load() >= kMaxInFlight || g_want || g_rec.buf) fprintf(stderr, "[shot] still saving earlier screenshots: request ignored\n");
    else { g_want = true; g_hud = hud_wanted(); g_armedAt = g_presentCount; }
  }
  if (g_rec.buf && !g_rec.hud && !g_shown.buf && copy_ok(src)) {   // this frame's HUD-less copy: keep the shown frame as a stand-in
    if (record_copy(h, cb, src, 0, 0, src->HW(), src->HH(), true, g_shown)) host_read_barrier(cb);
    return;
  }
  if (!g_want || g_rec.buf) return;
  const bool late = g_presentCount - g_armedAt >= kFallbackPresents;
  if (!g_hud && !late) return;   // ShotAfterDraw takes it at the next frame's tonemap draw
  if (!copy_ok(src)) { fprintf(stderr, "[shot] window texture format not supported: no screenshot\n"); g_want = false; return; }
  if (late && !g_hud) fprintf(stderr, "[shot] no 3D scene for %d presents: taking the screenshot with the HUD\n", kFallbackPresents);
  if (record_copy(h, cb, src, 0, 0, src->HW(), src->HH(), true, g_rec)) host_read_barrier(cb);
  g_want = false;
}

void ShotAfterDraw(ReplayCtx& cx, const char* key) {
  if (!g_want) return;   // unlocked peek (bool): the common case costs nothing
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_want || g_hud || g_rec.buf) return;
  TextureImpl* t = cx.rt[0];
  if (!copy_ok(t) || !AaIsTonemapKey(key)) return;
  EndRendering(cx);
  SubRange rr;
  t->RenderView(cx.rtHasView[0] ? &cx.rtView[0] : nullptr, &rr);
  if (rr.yn != 1) return;
  const uint32_t w = t->LevelW(rr.lb), hh = t->LevelH(rr.lb);
  if (!w || !hh) return;
  VkCommandBuffer cb = cx.cb;
  TransitionRange(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rr);
  const bool ok = record_copy(cx.h, cb, t, rr.lb, rr.yb, w, hh, false, g_rec);
  TransitionRange(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, rr);
  if (ok) host_read_barrier(cb);
  g_want = false;
  // only copies were recorded: the next draw re-begins rendering with the game's state as it was
}

void ShotAfterSubmit(Host* h, VkSemaphore timeline, uint64_t value) {
  Copy c, shown;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_rec.buf) return;
    c = g_rec; shown = g_shown;
    g_rec = Copy{}; g_shown = Copy{};
  }
  ++g_inFlight;
  const VkDevice dev = h->device;
  const std::string dir = shot_dir();
  ShotPost([c, shown, dev, dir, timeline, value]() mutable {
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1; wi.pSemaphores = &timeline; wi.pValues = &value;
    vkWaitSemaphores(dev, &wi, UINT64_MAX);
    if (!c.hud && shown.buf && blank(c)) {
      fprintf(stderr, "[shot] the 3D scene is empty in this frame (a menu): saving it as shown, with the HUD\n");
      std::swap(c, shown);
    }
    free_copy(dev, shown);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(widen(dir)), ec);
    SYSTEMTIME st; GetLocalTime(&st);
    char name[96];
    snprintf(name, sizeof name, "\\SnapRecomp_%04d%02d%02d_%02d%02d%02d_%d.png", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ++g_n);
    const std::string path = dir + name;
    const bool ok = ShotWritePng(path, static_cast<const uint8_t*>(c.map), c.w, c.h, c.bgra);
    fprintf(stderr, "[shot] screenshot %s %s (%ux%u, %s, present %d)\n", ok ? "saved" : "FAILED", path.c_str(), c.w, c.h, c.hud ? "with HUD" : "no HUD", c.present);
    free_copy(dev, c);
    --g_inFlight;
  });
}
}  // namespace nvn
