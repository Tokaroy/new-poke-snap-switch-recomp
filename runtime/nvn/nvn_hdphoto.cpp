// SPDX-License-Identifier: MIT
// HD photos: see nvn_hdphoto.h and analysis/core/hd_photos.md.
//
// Chain of one photo (all host side; the guest reads nothing from here):
//   shutter     CapturePS draws the scene into the 360x240 RGBA8 target T: HdPhotoCapture blits the region the draw samples
//               (from its texture coordinates) into a 2160x1440 RGBA8_SRGB image (the blit does the sRGB encode CapturePS
//               does), copies it to a host buffer (readback kind 4) and links T -> HD. The BC1 pass that packs T into the
//               thumbnail B links B -> HD (HdPhotoCompress); copies pass the link on. Photo select's large preview samples B:
//               HdPhotoSubstitute swaps in the HD texture.
//   submit      the game re-renders the photo (whole frame, 3:2 projection) into the 1080x720 target C: a new capture,
//   album save  linked C -> HD. The game copies C to a buffer and encodes the JPEG; snap_hdphoto_encoded finds the capture by
//               content (box averages) and the JPEG's CRC32 names the HD file, written q95 to <save root>\photos_hd\<crc>.jpg
//               (tmp file, flush, rename).
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
#include <cmath>
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
constexpr size_t kMaxFiles = 1200;        // well above the album's own limit; older HD files beyond it are deleted (oldest first)
constexpr int kMiniW = 135, kMiniH = 90;   // 16x16 HD texels = 8x8 photo texels per cell
constexpr float kMatchMax = 10.0f;        // mean abs difference (0..255) accepted as the same picture

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
// The album copies the decoded pixels into another buffer before the texture upload: decoded photos are also found by the
// hash of their first kHeadBytes (15 rows of 1080 RGBA pixels), checked when a 1080x720 texture is uploaded.
constexpr size_t kHeadBytes = 64 * 1024;
std::unordered_map<uint64_t, HdP> g_decHead;
uint64_t HeadHash(const uint8_t* p) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < kHeadBytes; i += 8) { uint64_t v; memcpy(&v, p + i, 8); h = (h ^ v) * 1099511628211ull; }
  return h;
}
std::vector<HdP> g_gpuLive;                             // HDs holding display textures (LRU by lastUse)
uint64_t g_nextId = 1, g_tick = 0;
TextureImpl* g_cap[2] = {};                             // the 2160x1440 blit targets (RGBA8, RGBA8_SRGB), reused (copies are ordered on the queue)

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

// Per-channel tone fit y = a*x + b from a capture's mini (x) to a game photo's mini (y), least squares over unclipped samples.
// The game's photo can differ from the shutter capture by a brightness/contrast step (CapturePS applies multiply/add after the
// sRGB encode); the fit removes it. Returns the mean absolute difference left after the fit (0..255).
struct Tone { float a[3] = {1, 1, 1}, b[3] = {0, 0, 0}; };
float FitDiff(const std::vector<uint8_t>& x, const std::vector<uint8_t>& y, Tone& t) {
  const size_t n = x.size() / 3;
  double s = 0;
  for (int c = 0; c < 3; c++) {
    double k = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; i++) {
      const int xi = x[i * 3 + c], yi = y[i * 3 + c];
      if (yi < 4 || yi > 251 || xi < 4 || xi > 251) continue;
      k++; sx += xi; sy += yi; sxx += (double)xi * xi; sxy += (double)xi * yi;
    }
    float a = 1.0f, b = 0.0f;
    const double den = k * sxx - sx * sx;
    if (k > (double)n / 4 && den > 1e-6) { a = (float)((k * sxy - sx * sy) / den); b = (float)((sy - a * sx) / k); }
    a = std::clamp(a, 0.5f, 2.0f); b = std::clamp(b, -96.0f, 96.0f);
    t.a[c] = a; t.b[c] = b;
    for (size_t i = 0; i < n; i++) s += std::fabs(std::clamp(a * x[i * 3 + c] + b, 0.0f, 255.0f) - (float)y[i * 3 + c]);
  }
  return (float)(s / (double)x.size());
}
bool ToneIsIdentity(const Tone& t) {
  for (int c = 0; c < 3; c++) if (std::fabs(t.a[c] - 1.0f) > 0.02f || std::fabs(t.b[c]) > 1.5f) return false;
  return true;
}

// Worker: the HD copy of a game photo whose tone differs from the shutter capture: src's pixels with the fitted tone, re-encoded
// into dst (dst->busy until done; the file is written when ready).
void PostTone(const HdP& src, const HdP& dst, const Tone& t) {
  dst->busy = true;
  Post([src, dst, t] {
    std::vector<uint8_t> px, jpg;
    { std::lock_guard<std::mutex> g(g_mu); if (!src->rgba.empty()) px = src->rgba; else jpg = src->jpg; }
    bool ok = !px.empty() || DecodeJpeg(jpg, px);
    if (ok) {
      uint8_t lut[3][256];
      for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++) lut[c][v] = (uint8_t)std::lround(std::clamp(t.a[c] * v + t.b[c], 0.0f, 255.0f));
      for (size_t i = 0; i + 3 < px.size(); i += 4) { px[i] = lut[0][px[i]]; px[i + 1] = lut[1][px[i + 1]]; px[i + 2] = lut[2][px[i + 2]]; }
      jpg.clear();
      ok = EncodeJpeg(px, jpg);
    }
    std::lock_guard<std::mutex> g(g_mu);
    dst->busy = false;
    if (!ok) { dst->bad = true; fprintf(stderr, "[hdphoto] tone-matched copy %08x failed: no HD copy\n", dst->crc); return; }
    dst->jpg = std::move(jpg);
    WriteIfReady(dst);
  });
}

// A game photo JPEG (1080x720) to tie to a session capture.
struct Waiting { uint32_t crc; size_t bytes; std::vector<uint8_t> mini; };
std::deque<Waiting> g_waiting;   // encoded before the readback of the capture that made them

// g_mu held. Finds the capture a game photo shows: closest after a per-channel tone fit (the game's photo can be a brightness
// step away from the capture). Accepted when the fitted difference is tiny, or small and clearly below every other capture's
// (a shot and its re-render can both be close). On success the HD file is written (re-toned when the fit is not identity).
// `final`: no more captures are coming, so a miss is logged.
bool MatchPhoto(const Waiting& w, bool final) {
  const std::vector<uint8_t>& m = w.mini;
  HdP best; float bd = 1e9f, second = 1e9f, raw = 0.0f; Tone bt;
  for (auto& hd : g_recent) {
    if (hd->mini.size() != m.size()) continue;
    Tone t;
    const float d = FitDiff(hd->mini, m, t);
    if (d < bd) {
      second = bd; bd = d; best = hd; bt = t;
      uint64_t sum = 0;
      for (size_t i = 0; i < m.size(); i++) sum += (uint64_t)std::abs((int)m[i] - (int)hd->mini[i]);
      raw = (float)sum / (float)m.size();
    } else if (d < second) second = d;
  }
  if (!best || !(bd <= 4.0f || (bd <= kMatchMax && bd <= 0.7f * second))) {
    if (final) {
      fprintf(stderr, "[hdphoto] album photo %08x: no matching capture (closest capture %llu: %.1f, %.1f after tone fit, next %.1f): no HD copy\n", w.crc,
              best ? (unsigned long long)best->id : 0ull, raw, bd, second);
      if (g_log) {   // diagnosis: the closest and the newest capture, next to the game's photo export
        for (const HdP& c : {best, g_recent.empty() ? HdP() : g_recent.back()}) {
          if (!c) continue;
          char n[64]; snprintf(n, sizeof n, "\\nomatch_%08x_capture%llu.jpg", w.crc, (unsigned long long)c->id);
          const std::string p = Dir() + n;
          Post([p, c] {
            std::vector<uint8_t> j, px;
            { std::lock_guard<std::mutex> g(g_mu); j = c->jpg; px = c->rgba; }
            if (j.empty() && !px.empty()) EncodeJpeg(px, j);
            CreateDirectoryA(Dir().c_str(), nullptr);
            if (!j.empty()) WriteAtomic(p, j);
          });
        }
      }
    }
    return false;
  }
  const bool tone = !ToneIsIdentity(bt);
  HdP hd = best;
  if (hd->crc || tone) {   // a second JPEG of one picture, or a re-toned one: its own HD object and file
    auto c = std::make_shared<Hd>();
    c->id = best->id; c->present = best->present; c->mini = best->mini;
    if (!tone) c->jpg = best->jpg;
    hd = c;
  }
  hd->crc = w.crc;
  g_byCrc[w.crc] = hd;
  g_noFile.erase(w.crc);
  fprintf(stderr, "[hdphoto] album photo %08x (%zu byte JPEG) is capture %llu (difference %.1f, %.1f after tone fit, next %.1f; tone R %.2f%+.0f G %.2f%+.0f B %.2f%+.0f)\n",
          w.crc, w.bytes, (unsigned long long)hd->id, raw, bd, second, bt.a[0], bt.b[0], bt.a[1], bt.b[1], bt.a[2], bt.b[2]);
  if (tone || (hd != best && hd->jpg.empty())) PostTone(best, hd, bt);   // re-toned, or the capture is still encoding
  else WriteIfReady(hd);
  return true;
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
// Blits host region (x0,y0)-(x1,y1) of src into the 2160x1440 capture image and queues its readback (kind 4). srgbDst: the
// capture image's format encodes (linear scene -> sRGB bytes, as CapturePS); otherwise the bytes are copied as they are.
HdP Grab(ReplayCtx& cx, TextureImpl* src, int x0, int y0, int x1, int y1, bool srgbDst) {
  if (!src->subLayout.empty() || src->b.samples > 1 || src->fi->bw != 1 || !(src->fi->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) return nullptr;
  VkFormatProperties fp{};
  vkGetPhysicalDeviceFormatProperties(src->host->phys, src->fi->vk, &fp);
  if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) { static int n = 0; if (n++ < 2) fprintf(stderr, "[hdphoto] scene format %d cannot be blitted\n", (int)src->fi->vk); return nullptr; }
  const bool linear = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
  TextureImpl*& cap = g_cap[srgbDst ? 1 : 0];
  if (!cap) {
    auto* s = new TextureImpl();
    s->host = src->host;
    s->b = TextureBuilder{};
    s->b.device = src->b.device; s->b.target = TT_2D; s->b.width = kW; s->b.height = kH; s->b.depth = 1; s->b.levels = 1;
    s->b.format = srgbDst ? FMT_RGBA8_SRGB : FMT_RGBA8; s->b.samples = 1;
    if (!s->EnsureImage()) { delete s; static int n = 0; if (n++ < 2) fprintf(stderr, "[hdphoto] capture image creation failed\n"); return nullptr; }
    s->uploaded = true;
    cap = s;
  }
  VkBuffer buf = VK_NULL_HANDLE;
  uint8_t* map = NewScratch(cx, kBytes, &buf);
  if (!map) return nullptr;
  EndRendering(cx);
  const VkImageLayout before = src->layout;
  Transition(cx.cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  Transition(cx.cb, cap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkImageBlit ib{};
  ib.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  ib.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  ib.srcOffsets[0] = {x0, y0, 0}; ib.srcOffsets[1] = {x1, y1, 1};
  ib.dstOffsets[0] = {0, 0, 0}; ib.dstOffsets[1] = {kW, kH, 1};
  vkCmdBlitImage(cx.cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cap->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ib, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
  Transition(cx.cb, cap, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy bc{};
  bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  bc.imageExtent = {(uint32_t)kW, (uint32_t)kH, 1};
  vkCmdCopyImageToBuffer(cx.cb, cap->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bc);
  HostReadBarrier(cx.cb);
  if (before != VK_IMAGE_LAYOUT_UNDEFINED) Transition(cx.cb, src, before);
  auto hd = std::make_shared<Hd>();
  {
    std::lock_guard<std::mutex> g(g_mu);
    hd->id = g_nextId++; hd->present = g_presentCount;
    g_pending[hd->id] = hd;
    if (g_pending.size() > 64) g_pending.erase(g_pending.begin());   // readbacks lost with a device error
  }
  Readback rb;
  rb.map = map; rb.dst = nullptr; rb.size = kBytes; rb.kind = 4; rb.poolOff = hd->id;
  rb.sb = cx.scratch->back();
  cx.scratch->pop_back();
  cx.readbacks->push_back(rb);
  return hd;
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

void HdPhotoCapture(ReplayCtx& cx, ProgramImpl* p, TextureImpl* src, TextureImpl* rt0, const float* c4, const float* uv) {
  const bool t360 = rt0->b.width == 360 && rt0->b.height == 240, t1080 = rt0->b.width == 1080 && rt0->b.height == 720;
  if (!src || !src->fi) return;
  if (!t360 && !t1080) {
    if (g_log && rt0->b.width >= 256 && rt0->b.width <= 2048) {   // diagnosis: capture passes into other targets
      const int k = CaptureProgKind(p);
      static int n = 0;
      if (k && n++ < 200) fprintf(stderr, "[hdphoto] capture kind %d into %dx%d (layers %d, target %d, host %ux%u) from %dx%d at present %d\n", k, rt0->b.width, rt0->b.height,
                                  rt0->b.depth, rt0->b.target, rt0->HW(), rt0->HH(), src->b.width, src->b.height, g_presentCount);
    }
    return;
  }
  const int kind = CaptureProgKind(p);
  if (kind != 2 && kind != 3) {
    static int n = 0;
    if (g_log && n++ < 300) fprintf(stderr, "[hdphoto] other draw into %dx%d fs=%s from %dx%d (host %ux%u f%d) at present %d\n", rt0->b.width, rt0->b.height, p->st[1].valid ? p->st[1].key : "-",
                                    src->b.width, src->b.height, src->HW(), src->HH(), (int)src->fi->vk, g_presentCount);
    return;
  }
  if (c4 && (c4[0] != 0.0f || c4[1] != 1.0f)) {   // {addValue, multiplyValue}: a faded capture; the plain blit cannot apply it
    static int n = 0; if (g_log || n++ < 4) fprintf(stderr, "[hdphoto] capture skipped: add %g multiply %g (%dx%d at present %d)\n", c4[0], c4[1], rt0->b.width, rt0->b.height, g_presentCount);
    std::lock_guard<std::mutex> g(g_mu); SetLink(rt0, nullptr); return;
  }
  // The sampled region: from the draw's texture coordinates when readable (the shutter samples the centered 3:2 part of the 16:9
  // scene; the album-save re-render samples a wider region), else the centered 3:2 crop.
  int x0, y0, x1, y1;
  const float hw = (float)src->HW(), hh = (float)src->HH();
  if (uv && uv[0] >= -0.001f && uv[1] >= -0.001f && uv[2] <= 1.001f && uv[3] <= 1.001f && uv[2] - uv[0] > 0.05f && uv[3] - uv[1] > 0.05f) {
    x0 = (int)std::lround(std::max(uv[0], 0.0f) * hw); y0 = (int)std::lround(std::max(uv[1], 0.0f) * hh);
    x1 = (int)std::lround(std::min(uv[2], 1.0f) * hw); y1 = (int)std::lround(std::min(uv[3], 1.0f) * hh);
  } else {
    if (uv && g_log) fprintf(stderr, "[hdphoto] capture texture coordinates (%.3f,%.3f)-(%.3f,%.3f) unusable: centered crop\n", uv[0], uv[1], uv[2], uv[3]);
    uv = nullptr;
    const float gw = (float)src->b.width, gh = (float)src->b.height;
    float cw = gw, ch = gw * 2.0f / 3.0f;
    if (ch > gh) { ch = gh; cw = gh * 1.5f; }
    const float sx = hw / gw, sy = hh / gh;
    x0 = (int)std::lround((gw - cw) * 0.5f * sx); y0 = (int)std::lround((gh - ch) * 0.5f * sy);
    x1 = (int)std::lround((gw + cw) * 0.5f * sx); y1 = (int)std::lround((gh + ch) * 0.5f * sy);
  }
  if (y1 - y0 < kH * 98 / 100) {   // region below 1440 host pixels tall (small source or a zoomed shot): an HD copy would only be an upscale
    static int n = 0; if (g_log || n++ < 2) fprintf(stderr, "[hdphoto] capture region %dx%d of %ux%u too small for a 2160x1440 copy: no HD copy\n", x1 - x0, y1 - y0, src->HW(), src->HH());
    std::lock_guard<std::mutex> g(g_mu); SetLink(rt0, nullptr); return;
  }
  HdP hd = Grab(cx, src, x0, y0, x1, y1, true);
  if (!hd) return;
  {
    std::lock_guard<std::mutex> g(g_mu);
    // The target shows this capture: the shutter's 360x240 thumbnail, or the 1080x720 photo the game re-renders for the score
    // screen and the album save (its JPEG matches this capture at 0.4/255 when the region comes from the draw's texture coordinates).
    SetLink(rt0, hd);
  }
  static int n = 0;
  if (g_log || n++ < 3)
    fprintf(stderr, "[hdphoto] capture %llu: %ux%u scene (%ux%u guest) region (%d,%d)-(%d,%d)%s -> 2160x1440 for the %dx%d %s %p at present %d\n", (unsigned long long)hd->id, src->HW(), src->HH(),
            src->b.width, src->b.height, x0, y0, x1, y1, uv ? " from the draw's texture coordinates" : "", rt0->b.width, rt0->b.height, t360 ? "thumbnail" : "photo", (const void*)rt0, g_presentCount);
}

// CopyTextureToTexture into a 1080x720 texture from a larger one (guest regions): a match candidate made the same way.
void HdPhotoCopyRegion(ReplayCtx& cx, TextureImpl* s, TextureImpl* d, int sx, int sy, int sw, int sh) {
  if (d->b.width != 1080 || d->b.height != 720 || !s->fi || s->b.width < 1080 || sw <= 0 || sh <= 0) return;
  const float kx = (float)s->HW() / (float)s->b.width, ky = (float)s->HH() / (float)s->b.height;
  const int x0 = (int)std::lround(sx * kx), y0 = (int)std::lround(sy * ky), x1 = (int)std::lround((sx + sw) * kx), y1 = (int)std::lround((sy + sh) * ky);
  if (g_log) fprintf(stderr, "[hdphoto] copy %p %dx%d (host %ux%u f%d) region (%d,%d %dx%d) -> 1080x720 %p at present %d\n", (const void*)s, s->b.width, s->b.height, s->HW(), s->HH(),
                     (int)s->fi->vk, sx, sy, sw, sh, (const void*)d, g_presentCount);
  if (y1 - y0 < kH * 98 / 100) return;
  const VkFormat f = s->fi->vk;
  const bool srgb = f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
  const bool unorm8 = f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_A8B8G8R8_UNORM_PACK32 || f == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  if (!srgb && !unorm8) return;   // a float scene copy would need the sRGB encode; only display-encoded sources are taken
  if (HdP hd = Grab(cx, s, x0, y0, x1, y1, srgb))
    if (g_log) fprintf(stderr, "[hdphoto] capture %llu from the copy\n", (unsigned long long)hd->id);
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
  for (auto w = g_waiting.begin(); w != g_waiting.end();) {   // photos encoded before this readback arrived
    if (MatchPhoto(*w, g_pending.empty()) || g_pending.empty()) w = g_waiting.erase(w);
    else ++w;
  }
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
  if (!hd && src && t->b.width == 1080 && t->b.height == 720 && !g_decHead.empty()) {   // a copy of a decoded photo's pixels
    auto it = g_decHead.find(HeadHash(src));
    if (it != g_decHead.end()) hd = it->second;
  }
  if (!hd && g_log && t->b.width == 1080 && t->b.height == 720 && !g_dec.empty()) {
    static int n = 0;
    if (n++ < 40) fprintf(stderr, "[hdphoto] 1080x720 texture %p uploaded from %p (no decoded photo there) at present %d\n", (const void*)t, (const void*)src, g_presentCount);
  }
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
  if (!hd && g_log && drawnW >= kMinDrawW && b.width == 1080) {   // diagnosis: a large photo without an HD link
    static std::unordered_set<const TextureImpl*> seen;
    if (seen.size() < 64 && seen.insert(t).second) {
      const uint8_t* addr = nullptr;
      if (b.pool) if (auto* pl = Impl<PoolImpl>(b.pool, MAGIC_POOL)) if (pl->storage) addr = static_cast<const uint8_t*>(pl->storage) + b.poolOffset;
      long long best = -1; const uint8_t* bk = nullptr;
      for (auto& [k, v] : g_dec) { const long long dd = std::llabs((long long)(k - addr)); if (addr && (best < 0 || dd < best)) { best = dd; bk = k; } }
      fprintf(stderr, "[hdphoto] large photo %p 1080x720 (fmt %d flags %x stride %d packaged %d) at %p without HD; nearest decoded buffer %p (%lld bytes away) at present %d\n",
              (const void*)t, (int)b.format, b.flags, b.stride, b.packaged != nullptr, (const void*)addr, (const void*)bk, best, g_presentCount);
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
  nvn::Waiting w{crc, n, {}};
  nvn::Mini(e->second.data, 1080, 720, 1080 * 4, w.mini);
  if (nvn::MatchPhoto(w, nvn::g_pending.empty()) || nvn::g_pending.empty()) return;
  // The game often encodes right after its own capture draw, before that capture's readback arrives: retry on each readback.
  nvn::g_waiting.push_back(std::move(w));
  if (nvn::g_waiting.size() > 8) nvn::g_waiting.pop_front();
  if (nvn::g_log) fprintf(stderr, "[hdphoto] album photo %08x: waiting for %zu capture readbacks\n", crc, nvn::g_pending.size());
}
// host/mods/shiny.cpp: a photo JPEG decoded into a guest buffer (the album). Records the buffer for the texture made from it.
extern "C" void snap_hdphoto_decoded(uint64_t dst, uint64_t size, uint32_t crc, int w, int h) {
  if (!nvn::HdPhotoOn() || !dst) return;
  std::lock_guard<std::mutex> g(nvn::g_mu);
  const uint8_t* p = reinterpret_cast<const uint8_t*>(dst);
  nvn::HdP hd = (w == 1080 && h == 720) ? nvn::ByCrc(crc) : nullptr;
  if (nvn::g_log) fprintf(stderr, "[hdphoto] decode %dx%d crc %08x into %p (%llu bytes)%s\n", w, h, crc, (const void*)p, (unsigned long long)size, hd ? ": has an HD copy" : "");
  if (!hd) { if (nvn::g_dec.erase(p)) nvn::UpdateActive(); return; }
  nvn::g_dec[p] = {size, crc, hd};
  if (nvn::g_dec.size() > 64) nvn::g_dec.erase(nvn::g_dec.begin());
  if (size >= nvn::kHeadBytes) {
    if (nvn::g_decHead.size() > 256) nvn::g_decHead.clear();
    nvn::g_decHead[nvn::HeadHash(p)] = hd;
  }
  nvn::UpdateActive();
  if (nvn::g_log) fprintf(stderr, "[hdphoto] decoded photo %08x into %p (%llu bytes): HD copy available\n", crc, (const void*)p, (unsigned long long)size);
}
extern "C" int snap_hdphoto_on() { return nvn::HdPhotoOn() ? 1 : 0; }
