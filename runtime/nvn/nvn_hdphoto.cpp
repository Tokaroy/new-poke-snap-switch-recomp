// SPDX-License-Identifier: MIT
// HD photos: see nvn_hdphoto.h and analysis/core/hd_photos.md.
//
// Chain of one photo (all host side; the guest reads nothing from here):
//   shutter     CapturePS draws the scene into the 360x240 RGBA8 target T: HdPhotoCapture blits the same region of the scene
//               into a 2160x1440 RGBA8_SRGB image (the blit does the sRGB encode CapturePS does), copies it to a host buffer
//               (readback kind 4) and links T -> HD. The BC1 pass that packs T into the thumbnail B links B -> HD
//               (HdPhotoCompress); copies pass the link on. Photo select's large preview samples B: HdPhotoSubstitute swaps in
//               the HD texture.
//   submit      the game re-renders the photo into the 1080x720 target C (same capture, linked C -> HD), copies C to a guest
//               buffer (HdPhotoCopyToBuffer: buffer -> HD), JpegEncoder::SetPixelData(buffer) and Encode: the album JPEG's
//               CRC32 names the HD file, written q95 to <save root>\photos_hd\<crc>.jpg (tmp file, flush, rename).
//   album       JpegDecoder::DecodeWithPreconfiguredDecoder (host/mods/shiny.cpp) reports (buffer, CRC); the texture made from
//               that buffer (CopyBufferToTexture, or a texture whose storage is the buffer) is linked to the CRC's HD copy,
//               loaded lazily from disk by a worker thread.
// Display textures are created on first large draw (sprite >= kMinDrawW UI units wide; small grid cells keep the game's texture,
// the HD image has no mip chain) and kept in an LRU of about kGpuBudget bytes. Missing or corrupt files fall back silently.
#include <windows.h>
#include <wincodec.h>
#include <objidl.h>

#include "nvn_internal.h"
#include "nvn_hdphoto.h"
#include "nvn_capture.h"
#include "hle/hle_config.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "windowscodecs")

namespace nvn {
namespace {
constexpr int kW = 2160, kH = 1440;
constexpr uint64_t kBytes = (uint64_t)kW * kH * 4;
constexpr uint64_t kGpuBudget = 256ull << 20;
constexpr float kMinDrawW = 480.0f;   // UI units (1920 wide): the photo select preview is ~1150, grid cells ~200
constexpr size_t kMaxFiles = 1200;
constexpr int kMiniW = 135, kMiniH = 90;   // 16x16 HD texels = 8x8 photo texels per cell
constexpr float kMatchMax = 10.0f;        // mean abs difference (0..255) accepted as the same picture    // the album's own limit; older HD files beyond it are deleted (oldest first)

struct Hd {
  uint64_t id = 0;
  int present = 0;
  std::vector<uint8_t> rgba;   // kW x kH, sRGB-encoded R,G,B,A bytes (empty while only the JPEG is held)
  std::vector<uint8_t> jpg;    // q95 copy of a session capture (written to disk once its album CRC is known)
  uint32_t crc = 0;            // album JPEG CRC32 (0 = not saved)
  bool fromDisk = false, busy = false, bad = false, written = false;
  TextureImpl* gpu[2] = {};    // display textures: [0] RGBA8 UNORM, [1] RGBA8 SRGB (matches the replaced texture's encoding)
  uint64_t lastUse = 0;
  std::vector<uint8_t> mini;   // kMiniW x kMiniH RGB box average: finds the capture an album JPEG was made from
};
using HdP = std::shared_ptr<Hd>;

bool g_log = false;
std::mutex g_mu;
std::atomic<int> g_active{0};                            // links + decoded buffers: zero keeps HdPhotoSubstitute a load + compare
std::unordered_map<const TextureImpl*, HdP> g_link;     // guest texture -> HD copy of what it shows
std::map<uint64_t, HdP> g_pending;                      // capture id -> HD waiting for its readback
std::deque<HdP> g_recent;                               // the session's captures (shots and photo re-renders), newest last
struct EncPix { const uint8_t* data; int w, h; };
std::unordered_map<uint64_t, EncPix> g_encBuf;          // JpegEncoder object -> its pixel buffer
std::unordered_map<uint32_t, HdP> g_byCrc;
std::unordered_set<uint32_t> g_noFile;                  // CRCs checked and without an HD file
struct Dec { uint64_t size; uint32_t crc; HdP hd; };
std::map<const uint8_t*, Dec> g_dec;                    // decoded album photo buffers (latest decode per buffer)
std::vector<HdP> g_gpuLive;                             // HDs holding display textures (LRU by lastUse)
uint64_t g_nextId = 1, g_tick = 0;
TextureImpl* g_cap = nullptr;                           // the 2160x1440 blit target, reused (copies are ordered on the queue)

// Box average of an RGBA image into kMiniW x kMiniH RGB (w, h multiples of the grid).
void Mini(const uint8_t* rgba, int w, int h, size_t stride, std::vector<uint8_t>& out) {
  const int bx = w / kMiniW, by = h / kMiniH;
  out.assign((size_t)kMiniW * kMiniH * 3, 0);
  for (int my = 0; my < kMiniH; my++)
    for (int mx = 0; mx < kMiniW; mx++) {
      uint32_t acc[3] = {};
      for (int y = my * by; y < (my + 1) * by; y++) {
        const uint8_t* r = rgba + (size_t)y * stride + (size_t)mx * bx * 4;
        for (int x = 0; x < bx; x++) { acc[0] += r[x * 4]; acc[1] += r[x * 4 + 1]; acc[2] += r[x * 4 + 2]; }
      }
      for (int c = 0; c < 3; c++) out[((size_t)my * kMiniW + mx) * 3 + c] = (uint8_t)(acc[c] / (uint32_t)(bx * by));
    }
}

void UpdateActive() { g_active.store((int)(g_link.size() + g_dec.size()), std::memory_order_relaxed); }

const std::string& Dir() {
  static const std::string d = [] {
    std::string r = hle::config().save_root;
    if (r.empty()) { const char* a = getenv("APPDATA"); r = std::string(a ? a : ".") + "\\SnapRecomp\\save"; }
    return r + "\\photos_hd";
  }();
  return d;
}
std::string PathOf(uint32_t crc) { char n[32]; snprintf(n, sizeof n, "\\%08x.jpg", crc); return Dir() + n; }

uint32_t Crc32(const uint8_t* p, size_t n) {
  static uint32_t t[256]; static std::once_flag once;
  std::call_once(once, [] { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; } });
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 255] ^ (c >> 8);
  return ~c;
}

// ---- worker thread (JPEG encode / decode / file IO; BelowNormal) ----
std::mutex g_qmu;
std::condition_variable g_qcv;
std::deque<std::function<void()>> g_q;
void Post(std::function<void()> f) {
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      for (;;) {
        std::function<void()> j;
        { std::unique_lock<std::mutex> lk(g_qmu); g_qcv.wait(lk, [] { return !g_q.empty(); }); j = std::move(g_q.front()); g_q.pop_front(); }
        j();
      }
    }).detach();
  });
  { std::lock_guard<std::mutex> lk(g_qmu); g_q.push_back(std::move(f)); }
  g_qcv.notify_one();
}

IWICImagingFactory* Wic() {   // worker thread only
  static IWICImagingFactory* f = nullptr;
  if (!f && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) f = nullptr;
  return f;
}

bool EncodeJpeg(const std::vector<uint8_t>& rgba, std::vector<uint8_t>& out) {
  IWICImagingFactory* f = Wic();
  if (!f) return false;
  IStream* ms = nullptr;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &ms))) return false;
  IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* pb = nullptr;
  bool ok = false;
  std::vector<uint8_t> bgr((size_t)kW * kH * 3);
  for (size_t i = 0, n = (size_t)kW * kH; i < n; i++) { bgr[i * 3] = rgba[i * 4 + 2]; bgr[i * 3 + 1] = rgba[i * 4 + 1]; bgr[i * 3 + 2] = rgba[i * 4]; }
  if (SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) && SUCCEEDED(enc->Initialize(ms, WICBitmapEncoderNoCache)) &&
      SUCCEEDED(enc->CreateNewFrame(&fr, &pb))) {
    PROPBAG2 opt{}; opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = 0.95f;
    pb->Write(1, &opt, &v);
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(fr->Initialize(pb)) && SUCCEEDED(fr->SetSize(kW, kH)) && SUCCEEDED(fr->SetPixelFormat(&pf)) && pf == GUID_WICPixelFormat24bppBGR &&
        SUCCEEDED(fr->WritePixels(kH, kW * 3, (UINT)bgr.size(), bgr.data())) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit())) {
      STATSTG st{};
      LARGE_INTEGER z{};
      if (SUCCEEDED(ms->Stat(&st, STATFLAG_NONAME)) && st.cbSize.QuadPart > 0 && st.cbSize.QuadPart < (64ull << 20) && SUCCEEDED(ms->Seek(z, STREAM_SEEK_SET, nullptr))) {
        out.resize((size_t)st.cbSize.QuadPart);
        ULONG got = 0;
        ok = SUCCEEDED(ms->Read(out.data(), (ULONG)out.size(), &got)) && got == out.size();
      }
    }
  }
  if (pb) pb->Release();
  if (fr) fr->Release();
  if (enc) enc->Release();
  ms->Release();
  return ok;
}

bool DecodeJpeg(const std::vector<uint8_t>& jpg, std::vector<uint8_t>& rgba) {
  IWICImagingFactory* f = Wic();
  if (!f || jpg.size() < 4) return false;
  IWICStream* st = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
  bool ok = false;
  UINT w = 0, h = 0;
  if (SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory(const_cast<BYTE*>(jpg.data()), (DWORD)jpg.size())) &&
      SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
      SUCCEEDED(fr->GetSize(&w, &h)) && w == (UINT)kW && h == (UINT)kH && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
      SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
    rgba.resize(kBytes);
    ok = SUCCEEDED(cv->CopyPixels(nullptr, kW * 4, (UINT)rgba.size(), rgba.data()));
  }
  if (cv) cv->Release();
  if (fr) fr->Release();
  if (dec) dec->Release();
  if (st) st->Release();
  if (!ok) rgba.clear();
  return ok;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
  bool ok = n > 0 && n < (64l << 20);
  if (ok) { out.resize((size_t)n); ok = fread(out.data(), 1, (size_t)n, f) == (size_t)n; }
  fclose(f);
  return ok;
}

// Same as the saves: a temp file, flushed to disk, then renamed over the target.
bool WriteAtomic(const std::string& path, const std::vector<uint8_t>& data) {
  CreateDirectoryA(Dir().c_str(), nullptr);
  const std::string tmp = path + ".tmp";
  HANDLE h = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD wr = 0;
  bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr) && wr == data.size() && FlushFileBuffers(h);
  CloseHandle(h);
  ok = ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  if (!ok) DeleteFileA(tmp.c_str());
  return ok;
}

// Keeps at most kMaxFiles HD files (newest by write time) and removes stale temp files. Worker thread, once per session.
void TrimDir() {
  struct F { std::string name; FILETIME t; };
  std::vector<F> fs;
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((Dir() + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const std::string n = fd.cFileName;
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".tmp") == 0) { DeleteFileA((Dir() + "\\" + n).c_str()); continue; }
    if (n.size() == 12 && n.compare(8, 4, ".jpg") == 0) fs.push_back({n, fd.ftLastWriteTime});
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  if (fs.size() <= kMaxFiles) return;
  std::sort(fs.begin(), fs.end(), [](const F& a, const F& b) { return CompareFileTime(&a.t, &b.t) > 0; });
  for (size_t i = kMaxFiles; i < fs.size(); i++) DeleteFileA((Dir() + "\\" + fs[i].name).c_str());
  fprintf(stderr, "[hdphoto] trimmed %zu old HD files (limit %zu)\n", fs.size() - kMaxFiles, kMaxFiles);
}

void WriteIfReady(const HdP& hd) {   // g_mu held
  if (!hd->crc || hd->written || hd->jpg.empty() || hd->fromDisk) return;
  hd->written = true;
  const uint32_t crc = hd->crc;
  std::shared_ptr<std::vector<uint8_t>> data = std::make_shared<std::vector<uint8_t>>(hd->jpg);
  Post([crc, data] {
    static std::once_flag trim;
    std::call_once(trim, TrimDir);
    const std::string p = PathOf(crc);
    const bool ok = WriteAtomic(p, *data);
    fprintf(stderr, "[hdphoto] %s %s (%zu bytes)\n", ok ? "wrote" : "FAILED to write", p.c_str(), data->size());
  });
}

// Worker: session capture -> q95 JPEG (kept in memory; the raw pixels are dropped unless a display texture still needs them).
void PostEncode(const HdP& hd) {
  hd->busy = true;
  Post([hd] {
    std::vector<uint8_t> px;
    { std::lock_guard<std::mutex> g(g_mu); px = hd->rgba; }
    std::vector<uint8_t> jpg;
    const bool ok = !px.empty() && EncodeJpeg(px, jpg);
    std::lock_guard<std::mutex> g(g_mu);
    hd->busy = false;
    if (!ok) { hd->bad = true; fprintf(stderr, "[hdphoto] JPEG encode failed (capture %llu)\n", (unsigned long long)hd->id); return; }
    hd->jpg = std::move(jpg);
    if (!hd->gpu[0] && !hd->gpu[1]) { hd->rgba.clear(); hd->rgba.shrink_to_fit(); }
    if (g_log) fprintf(stderr, "[hdphoto] capture %llu encoded: %zu bytes\n", (unsigned long long)hd->id, hd->jpg.size());
    WriteIfReady(hd);
  });
}

// Worker: pixels for a display texture, from the in-memory JPEG or the file. Failure marks the HD bad (the game's texture stays).
void PostLoad(const HdP& hd) {
  hd->busy = true;
  Post([hd] {
    std::vector<uint8_t> jpg;
    bool disk;
    { std::lock_guard<std::mutex> g(g_mu); jpg = hd->jpg; disk = hd->fromDisk; }
    if (jpg.empty() && disk) ReadFile(PathOf(hd->crc), jpg);
    std::vector<uint8_t> px;
    const bool ok = DecodeJpeg(jpg, px);
    std::lock_guard<std::mutex> g(g_mu);
    hd->busy = false;
    if (!ok) { hd->bad = true; fprintf(stderr, "[hdphoto] HD copy %08x unreadable: showing the game's photo\n", hd->crc); return; }
    hd->rgba = std::move(px);
    if (g_log) fprintf(stderr, "[hdphoto] loaded HD copy %08x (%zu byte JPEG)\n", hd->crc, jpg.size());
  });
}

HdP ByCrc(uint32_t crc) {   // g_mu held; nullptr when no HD file exists
  auto it = g_byCrc.find(crc);
  if (it != g_byCrc.end()) return it->second;
  if (g_noFile.count(crc)) return nullptr;
  if (GetFileAttributesA(PathOf(crc).c_str()) == INVALID_FILE_ATTRIBUTES) { g_noFile.insert(crc); return nullptr; }
  auto hd = std::make_shared<Hd>();
  hd->id = g_nextId++; hd->crc = crc; hd->fromDisk = true; hd->written = true;
  g_byCrc[crc] = hd;
  return hd;
}

void SetLink(const TextureImpl* t, const HdP& hd) { if (hd) g_link[t] = hd; else g_link.erase(t); UpdateActive(); }

void EvictLocked(const HdP& keep) {
  uint64_t bytes = 0;
  for (auto& h : g_gpuLive) for (auto* t : h->gpu) if (t) bytes += kBytes;
  while (bytes > kGpuBudget && g_gpuLive.size() > 1) {
    auto lru = std::min_element(g_gpuLive.begin(), g_gpuLive.end(), [&](const HdP& a, const HdP& b) { return (a == keep ? UINT64_MAX : a->lastUse) < (b == keep ? UINT64_MAX : b->lastUse); });
    if (*lru == keep) break;
    for (auto*& t : (*lru)->gpu) if (t) { Retire(GK_TEXTURE, t); t = nullptr; bytes -= kBytes; }   // freed once the GPU is past it
    if (g_log) fprintf(stderr, "[hdphoto] evicted display texture of capture %llu\n", (unsigned long long)(*lru)->id);
    g_gpuLive.erase(lru);
  }
}

void HostReadBarrier(VkCommandBuffer cb) {
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cb, &di);
}
}  // namespace

bool HdPhotoOn() {
  static const bool on = [] {
    const char* e = getenv("SNAP_HD_PHOTOS");
    const bool v = e && (*e == '1' || !_stricmp(e, "on") || !_stricmp(e, "true") || !_stricmp(e, "yes"));
    g_log = getenv("SNAP_HD_PHOTOS_LOG") != nullptr;
    if (v) fprintf(stderr, "[hdphoto] HD photos on: 2160x1440 copies in %s\n", Dir().c_str());
    return v;
  }();
  return on;
}

void HdPhotoCapture(ReplayCtx& cx, ProgramImpl* p, TextureImpl* src, TextureImpl* rt0, const float* c4) {
  const bool t360 = rt0->b.width == 360 && rt0->b.height == 240, t1080 = rt0->b.width == 1080 && rt0->b.height == 720;
  if ((!t360 && !t1080) || !src || !src->fi) return;
  const int kind = CaptureProgKind(p);
  if (kind != 2 && kind != 3) return;
  if (c4 && (c4[0] != 0.0f || c4[1] != 1.0f)) {   // {addValue, multiplyValue}: a faded capture; the plain blit cannot apply it
    static int n = 0; if (g_log || n++ < 4) fprintf(stderr, "[hdphoto] capture skipped: add %g multiply %g (%dx%d at present %d)\n", c4[0], c4[1], rt0->b.width, rt0->b.height, g_presentCount);
    std::lock_guard<std::mutex> g(g_mu); SetLink(rt0, nullptr); return;
  }
  // The photo is the centered 3:2 part of the 16:9 scene (checked against the game's own 1080x720 photo, analysis/core/hd_photos.md).
  const float gw = (float)src->b.width, gh = (float)src->b.height;
  float cw = gw, ch = gw * 2.0f / 3.0f;
  if (ch > gh) { ch = gh; cw = gh * 1.5f; }
  const float sx = (float)src->HW() / gw, sy = (float)src->HH() / gh;
  const int x0 = (int)std::lround((gw - cw) * 0.5f * sx), y0 = (int)std::lround((gh - ch) * 0.5f * sy);
  const int x1 = (int)std::lround((gw + cw) * 0.5f * sx), y1 = (int)std::lround((gh + ch) * 0.5f * sy);
  if (y1 - y0 < kH * 98 / 100) {   // source below 2160x1440: an HD copy would only be an upscale
    static int n = 0; if (n++ < 2) fprintf(stderr, "[hdphoto] capture source %ux%u too small for a 2160x1440 copy: HD photos inactive\n", src->HW(), src->HH());
    std::lock_guard<std::mutex> g(g_mu); SetLink(rt0, nullptr); return;
  }
  if (!src->subLayout.empty() || src->b.samples > 1 || src->fi->bw != 1 || !(src->fi->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) return;
  VkFormatProperties fp{};
  vkGetPhysicalDeviceFormatProperties(src->host->phys, src->fi->vk, &fp);
  if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) { static int n = 0; if (n++ < 2) fprintf(stderr, "[hdphoto] scene format %d cannot be blitted\n", (int)src->fi->vk); return; }
  const bool linear = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
  if (!g_cap) {
    auto* s = new TextureImpl();
    s->host = src->host;
    s->b = TextureBuilder{};
    s->b.device = src->b.device; s->b.target = TT_2D; s->b.width = kW; s->b.height = kH; s->b.depth = 1; s->b.levels = 1;
    s->b.format = FMT_RGBA8_SRGB; s->b.samples = 1;
    if (!s->EnsureImage()) { delete s; static int n = 0; if (n++ < 2) fprintf(stderr, "[hdphoto] capture image creation failed\n"); return; }
    s->uploaded = true;
    g_cap = s;
  }
  VkBuffer buf = VK_NULL_HANDLE;
  uint8_t* map = NewScratch(cx, kBytes, &buf);
  if (!map) return;
  EndRendering(cx);
  const VkImageLayout before = src->layout;
  Transition(cx.cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  Transition(cx.cb, g_cap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkImageBlit ib{};
  ib.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  ib.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  ib.srcOffsets[0] = {x0, y0, 0}; ib.srcOffsets[1] = {x1, y1, 1};
  ib.dstOffsets[0] = {0, 0, 0}; ib.dstOffsets[1] = {kW, kH, 1};
  vkCmdBlitImage(cx.cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_cap->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ib, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
  Transition(cx.cb, g_cap, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy bc{};
  bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  bc.imageExtent = {(uint32_t)kW, (uint32_t)kH, 1};
  vkCmdCopyImageToBuffer(cx.cb, g_cap->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bc);
  HostReadBarrier(cx.cb);
  if (before != VK_IMAGE_LAYOUT_UNDEFINED) Transition(cx.cb, src, before);
  auto hd = std::make_shared<Hd>();
  {
    std::lock_guard<std::mutex> g(g_mu);
    hd->id = g_nextId++; hd->present = g_presentCount;
    g_pending[hd->id] = hd;
    SetLink(rt0, hd);
    if (g_pending.size() > 64) g_pending.erase(g_pending.begin());   // readbacks lost with a device error
  }
  Readback rb;
  rb.map = map; rb.dst = nullptr; rb.size = kBytes; rb.kind = 4; rb.poolOff = hd->id;
  rb.sb = cx.scratch->back();
  cx.scratch->pop_back();
  cx.readbacks->push_back(rb);
  static int n = 0;
  if (g_log || n++ < 3)
    fprintf(stderr, "[hdphoto] capture %llu: %ux%u scene (%ux%u guest) region (%d,%d)-(%d,%d) -> 2160x1440 (%s) for the %dx%d %s at present %d\n", (unsigned long long)hd->id, src->HW(), src->HH(),
            src->b.width, src->b.height, x0, y0, x1, y1, linear ? "linear" : "nearest", rt0->b.width, rt0->b.height, t360 ? "thumbnail" : "photo", g_presentCount);
}

void HdPhotoReadback(uint64_t id, const uint8_t* map, uint64_t size) {
  std::lock_guard<std::mutex> g(g_mu);
  auto it = g_pending.find(id);
  if (it == g_pending.end()) return;
  HdP hd = it->second;
  g_pending.erase(it);
  if (size != kBytes) return;
  hd->rgba.assign(map, map + size);
  Mini(hd->rgba.data(), kW, kH, (size_t)kW * 4, hd->mini);
  g_recent.push_back(hd);
  if (g_recent.size() > 160) g_recent.pop_front();   // more than one expedition of shots
  PostEncode(hd);
}

void HdPhotoCompress(const TextureImpl* bc, const TextureImpl* src) {
  std::lock_guard<std::mutex> g(g_mu);
  HdP hd;
  if (src) { auto it = g_link.find(src); if (it != g_link.end()) hd = it->second; }
  if (!hd && !g_link.count(bc)) return;
  SetLink(bc, hd);
  if (g_log && hd) fprintf(stderr, "[hdphoto] thumbnail %p (BC %dx%d) shows capture %llu\n", (const void*)bc, bc->b.width, bc->b.height, (unsigned long long)hd->id);
}

void HdPhotoCopyTex(const TextureImpl* s, const TextureImpl* d) {
  std::lock_guard<std::mutex> g(g_mu);
  auto it = g_link.find(s);
  if (it == g_link.end()) { if (g_link.count(d)) SetLink(d, nullptr); return; }
  SetLink(d, it->second);
  if (g_log) fprintf(stderr, "[hdphoto] copy %p -> %p (%dx%d) keeps capture %llu\n", (const void*)s, (const void*)d, d->b.width, d->b.height, (unsigned long long)it->second->id);
}

void HdPhotoCopyToBuffer(const TextureImpl* t, const uint8_t* dst) {   // log only: the encoder reads another buffer (content match below)
  if (g_log && t->b.width == 1080 && t->b.height == 720) fprintf(stderr, "[hdphoto] photo copy %p -> buffer %p at present %d\n", (const void*)t, (const void*)dst, g_presentCount);
}

void HdPhotoUploadFrom(const TextureImpl* t, const uint8_t* src) {
  std::lock_guard<std::mutex> g(g_mu);
  HdP hd;
  if (src) { auto it = g_dec.find(src); if (it != g_dec.end()) hd = it->second.hd; }
  if (!hd && !g_link.count(t)) return;
  SetLink(t, hd);
  if (g_log && hd) fprintf(stderr, "[hdphoto] texture %p (%dx%d) uploaded from decoded photo %08x\n", (const void*)t, t->b.width, t->b.height, hd->crc);
}

void HdPhotoTexGone(const TextureImpl* t) {
  std::lock_guard<std::mutex> g(g_mu);
  if (g_link.erase(t)) UpdateActive();
}

TextureImpl* HdPhotoSubstitute(TextureImpl* t, float drawnW) {
  if (g_active.load(std::memory_order_relaxed) == 0) return nullptr;
  const TextureBuilder& b = t->b;
  if (!((b.width == 360 && b.height == 240) || (b.width == 1080 && b.height == 720))) return nullptr;
  std::lock_guard<std::mutex> g(g_mu);
  HdP hd;
  auto it = g_link.find(t);
  if (it != g_link.end()) hd = it->second;
  else if (!g_dec.empty() && b.pool && !b.packaged) {   // a texture whose storage is a decoded album photo buffer
    if (auto* pl = Impl<PoolImpl>(b.pool, MAGIC_POOL)) if (pl->storage) {
      auto d = g_dec.find(static_cast<const uint8_t*>(pl->storage) + b.poolOffset);
      if (d != g_dec.end()) hd = d->second.hd;
    }
  }
  if (!hd || hd->bad) return nullptr;
  if (drawnW < kMinDrawW) return nullptr;
  hd->lastUse = ++g_tick;
  const int fi = (t->fi && (t->fi->vk == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || t->fi->vk == VK_FORMAT_BC1_RGB_SRGB_BLOCK || t->fi->vk == VK_FORMAT_R8G8B8A8_SRGB ||
                            t->fi->vk == VK_FORMAT_B8G8R8A8_SRGB || t->fi->vk == VK_FORMAT_BC3_SRGB_BLOCK || t->fi->vk == VK_FORMAT_BC7_SRGB_BLOCK)) ? 1 : 0;
  if (TextureImpl* s = hd->gpu[fi]) {
    if (s->uploaded && s->b.packaged) {   // uploaded: the pixels are no longer needed
      s->b.packaged = nullptr;
      if (!hd->busy && !hd->gpu[fi ^ 1] && !hd->jpg.empty()) { hd->rgba.clear(); hd->rgba.shrink_to_fit(); }
    }
    return s;
  }
  if (hd->rgba.empty()) { if (!hd->busy) PostLoad(hd); return nullptr; }
  auto* s = new TextureImpl();
  s->host = t->host;
  s->b = TextureBuilder{};
  s->b.device = b.device; s->b.target = TT_2D; s->b.width = kW; s->b.height = kH; s->b.depth = 1; s->b.levels = 1; s->b.samples = 1;
  s->b.format = fi ? FMT_RGBA8_SRGB : FMT_RGBA8;
  s->b.flags = TF_LINEAR; s->b.stride = kW * 4; s->b.packaged = hd->rgba.data();
  s->shTried = true;
  if (!s->EnsureImage()) { delete s; hd->bad = true; return nullptr; }
  hd->gpu[fi] = s;
  if (std::find(g_gpuLive.begin(), g_gpuLive.end(), hd) == g_gpuLive.end()) g_gpuLive.push_back(hd);
  EvictLocked(hd);
  static int n = 0;
  if (g_log || n++ < 6) fprintf(stderr, "[hdphoto] showing HD copy of %s %08x (capture %llu) for %p %dx%d drawn %.0f wide at present %d\n", hd->crc ? "photo" : "shot", hd->crc,
                                (unsigned long long)hd->id, (const void*)t, b.width, b.height, drawnW, g_presentCount);
  return s;
}
}  // namespace nvn

// host/mods/photoexport.cpp: JpegEncoder::SetPixelData / Encode (every call; returns at once when HD photos are off).
extern "C" void snap_hdphoto_pixels(uint64_t enc, uint64_t data, const int* dim) {
  if (!nvn::HdPhotoOn()) return;
  std::lock_guard<std::mutex> g(nvn::g_mu);
  nvn::g_encBuf[enc] = {reinterpret_cast<const uint8_t*>(data), dim ? dim[0] : 0, dim ? dim[1] : 0};
}
// A successful Encode of a 1080x720 photo: the session capture whose picture it is (closest box average, under kMatchMax) gets
// the JPEG's CRC32 as its file name. Shots match their shutter capture; the submit re-render is a candidate too.
extern "C" void snap_hdphoto_encoded(uint64_t enc, const uint8_t* jpg, size_t n) {
  if (!nvn::HdPhotoOn() || !jpg || n < 4 || n > (64u << 20)) return;
  std::lock_guard<std::mutex> g(nvn::g_mu);
  auto e = nvn::g_encBuf.find(enc);
  if (e == nvn::g_encBuf.end() || e->second.w != 1080 || e->second.h != 720 || !e->second.data) return;
  const uint32_t crc = nvn::Crc32(jpg, n);
  if (nvn::g_byCrc.count(crc)) return;   // the same photo encoded again
  std::vector<uint8_t> m;
  nvn::Mini(e->second.data, 1080, 720, 1080 * 4, m);
  nvn::HdP best; float bd = 1e9f, second = 1e9f;
  for (auto& hd : nvn::g_recent) {
    if (hd->mini.size() != m.size()) continue;
    uint64_t sum = 0;
    for (size_t i = 0; i < m.size(); i++) sum += (uint64_t)std::abs((int)m[i] - (int)hd->mini[i]);
    const float d = (float)sum / (float)m.size();
    if (d < bd) { second = bd; bd = d; best = hd; } else if (d < second) second = d;
  }
  if (!best || bd > nvn::kMatchMax) { fprintf(stderr, "[hdphoto] album photo %08x: no matching capture (closest %.1f): no HD copy\n", crc, bd); return; }
  nvn::HdP hd = best;
  if (hd->crc) { auto c = std::make_shared<nvn::Hd>(*hd); c->gpu[0] = c->gpu[1] = nullptr; c->written = false; hd = c; }   // one picture, a second JPEG: its own file
  hd->crc = crc;
  nvn::g_byCrc[crc] = hd;
  nvn::g_noFile.erase(crc);
  fprintf(stderr, "[hdphoto] album photo %08x (%zu byte JPEG) is capture %llu (difference %.1f, next %.1f)\n", crc, n, (unsigned long long)hd->id, bd, second);
  nvn::WriteIfReady(hd);
}
// host/mods/shiny.cpp: a photo JPEG decoded into a guest buffer (the album). Records the buffer for the texture made from it.
extern "C" void snap_hdphoto_decoded(uint64_t dst, uint64_t size, uint32_t crc, int w, int h) {
  if (!nvn::HdPhotoOn() || !dst) return;
  std::lock_guard<std::mutex> g(nvn::g_mu);
  const uint8_t* p = reinterpret_cast<const uint8_t*>(dst);
  nvn::HdP hd = (w == 1080 && h == 720) ? nvn::ByCrc(crc) : nullptr;
  if (!hd) { if (nvn::g_dec.erase(p)) nvn::UpdateActive(); return; }
  nvn::g_dec[p] = {size, crc, hd};
  if (nvn::g_dec.size() > 64) nvn::g_dec.erase(nvn::g_dec.begin());
  nvn::UpdateActive();
  if (nvn::g_log) fprintf(stderr, "[hdphoto] decoded photo %08x into %p (%llu bytes): HD copy available\n", crc, (const void*)p, (unsigned long long)size);
}
extern "C" int snap_hdphoto_on() { return nvn::HdPhotoOn() ? 1 : 0; }
