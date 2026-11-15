// SPDX-License-Identifier: MIT
// Button icon mod, renderer side (analysis/mods/button_icons.md): a UI draw that samples one of the game's Switch button sprites samples a
// generated twin instead, showing the Xbox or PlayStation glyph for the button the player really presses (runtime/hle/btn_icons.h).
//
//   key    = FNV-1a 64 of the whole swizzled data of a small 2D color texture (<= 64 KB); the same bytes as in the nutexb file
//            (scripts/mods/btnicon_keys.py computes it from the extracted files). nvn_btnicon_table.h lists the button sprites by key.
//   plate  the sprite is the whole button picture (a letter in a disc): the twin is our glyph alone, fitted into the sprite's shape.
//   pill   an orange chevron with a lettered backdrop: the twin is the original, decoded here from the user's own copy (never shipped),
//          with the backdrop erased and our glyph drawn over it.
// The art (nvn_btnicon_atlas.h, CC0 Kenney Input Prompts) is one PNG decoded once with WIC. A twin belongs to its parent texture
// (TextureImpl::biAlts) and dies with it. SNAP_BTNICON_TRACE=<file> logs every distinct small sprite UI draws sample (key, size, rect)
// so the table can be extended.
#include "nvn_btnicon.h"

#include <windows.h>
#include <objidl.h>
#include <wincodec.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "nvn_btnicon_atlas.h"
#include "nvn_btnicon_table.h"
#include "nvn_internal.h"
#include "nvn_tex.h"

#pragma comment(lib, "windowscodecs")

extern "C" int snap_btnicon_glyph(int button);   // runtime/hle/btn_icons.cpp: (row << 8) | glyph, or -1 = keep the game's icon
extern "C" int snap_btnicon_active();

namespace nvn {
namespace {
struct Cfg {
  FILE* trace = nullptr;
};
const Cfg& C() {
  static const Cfg c = [] {
    Cfg c;
    if (const char* e = getenv("SNAP_BTNICON_TRACE"); e && *e) c.trace = fopen(e, "w");
    return c;
  }();
  return c;
}
std::mutex g_mu;

uint64_t Fnv(const uint8_t* p, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }

// The guest bytes of a static texture (packaged data, or the pool it lives in), or null.
const uint8_t* GuestData(const TextureImpl* t) {
  const TextureBuilder& b = t->b;
  const uint8_t* src = static_cast<const uint8_t*>(b.packaged);
  if (!src && b.pool && !(b.flags & (TF_COMPRESSIBLE | TF_DISPLAY | TF_LINEAR_RENDER_TARGET | TF_LINEAR)))
    if (auto* p = Impl<PoolImpl>(b.pool, MAGIC_POOL)) if (p->storage) src = static_cast<const uint8_t*>(p->storage) + b.poolOffset;
  return src;
}
constexpr uint64_t kKeyMax = 65536;   // a sprite larger than this is not keyed (its bytes would cost a hash per texture)
// Key of a candidate texture (small 2D color texture), 0 when it is not one.
uint64_t KeyOf(const TextureImpl* t) {
  const TextureBuilder& b = t->b;
  if (t->scale != 1.0f || b.samples > 1 || b.target != TT_2D || b.width < 8 || b.height < 8 || b.width > 512 || b.height > 512) return 0;
  const FormatInfo* fi = LookupFormat(b.format);
  if (!fi || fi->astcW || fi->aspect != VK_IMAGE_ASPECT_COLOR_BIT) return 0;
  const uint8_t* src = GuestData(t);
  if (!src) return 0;
  const SurfaceLayout l = ComputeSurfaceLayout((uint32_t)b.width, (uint32_t)b.height, 1, 1, (uint32_t)std::max(b.levels, 1), fi->bytes, fi->bw, fi->bh);
  if (l.total > kKeyMax) return 0;   // glyph sprites are tiny; big art is never a glyph
  return Fnv(src, (size_t)l.total);
}

// ---- the glyph atlas: one PNG, 64x64 cells, row 0 Xbox, row 1 PlayStation; columns = hle::icons::Glyph
constexpr int kCell = 64, kGlyphs = 26;
struct Glyph {
  int w = 0, h = 0;                // size of the visible part (alpha > 8) plus a 1 px transparent border
  std::vector<float> pm;           // premultiplied RGBA, w*h*4
  bool ok() const { return w > 0; }
};
Glyph g_glyph[2][kGlyphs];
bool g_atlasTried = false;

bool DecodePng(const uint8_t* png, size_t n, std::vector<uint8_t>& rgba, int& w, int& h) {
  bool ok = false;
  std::thread([&] {   // WIC wants a COM apartment; the replay thread's is not ours to touch
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
    IWICImagingFactory* f = nullptr;
    IWICStream* st = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    UINT uw = 0, uh = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) && SUCCEEDED(f->CreateStream(&st)) &&
        SUCCEEDED(st->InitializeFromMemory(const_cast<BYTE*>(png), (DWORD)n)) &&
        SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
        SUCCEEDED(fr->GetSize(&uw, &uh)) && uw > 0 && uh > 0 && uw < 8192 && uh < 8192 && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
      rgba.resize((size_t)uw * uh * 4);
      w = (int)uw; h = (int)uh;
      ok = SUCCEEDED(cv->CopyPixels(nullptr, uw * 4, (UINT)rgba.size(), rgba.data()));
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    if (st) st->Release();
    if (f) f->Release();
    CoUninitialize();
  }).join();
  return ok;
}

void LoadAtlas() {   // g_mu held
  if (g_atlasTried) return;
  g_atlasTried = true;
  std::vector<uint8_t> px; int aw = 0, ah = 0;
  if (!DecodePng(kAtlasPng, sizeof kAtlasPng, px, aw, ah) || aw != kCell * kGlyphs || ah != kCell * 2) {
    fprintf(stderr, "[btnicon] atlas decode failed (%dx%d); button icons stay as the game ships\n", aw, ah);
    return;
  }
  for (int row = 0; row < 2; row++)
    for (int g = 0; g < kGlyphs; g++) {
      int x0 = kCell, y0 = kCell, x1 = -1, y1 = -1;
      for (int y = 0; y < kCell; y++)
        for (int x = 0; x < kCell; x++)
          if (px[((size_t)(row * kCell + y) * aw + g * kCell + x) * 4 + 3] > 8) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); }
      if (x1 < 0) continue;
      Glyph& gl = g_glyph[row][g];
      gl.w = x1 - x0 + 1 + 2; gl.h = y1 - y0 + 1 + 2;
      gl.pm.assign((size_t)gl.w * gl.h * 4, 0.0f);
      for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
          const uint8_t* p = &px[((size_t)(row * kCell + y) * aw + g * kCell + x) * 4];
          float* d = &gl.pm[((size_t)(y - y0 + 1) * gl.w + (x - x0 + 1)) * 4];
          const float a = p[3] / 255.0f;
          d[0] = p[0] / 255.0f * a; d[1] = p[1] / 255.0f * a; d[2] = p[2] / 255.0f * a; d[3] = a;
        }
    }
}

// ---- pixel work
struct Img {
  int w = 0, h = 0;
  std::vector<uint8_t> px;   // straight RGBA8
};

// Bilinear sample of a glyph (premultiplied), u,v in glyph pixels (pixel centers at +0.5).
void SampleGlyph(const Glyph& g, float u, float v, float out[4]) {
  const float fx = u - 0.5f, fy = v - 0.5f;
  const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
  const float tx = fx - x0, ty = fy - y0;
  out[0] = out[1] = out[2] = out[3] = 0;
  for (int j = 0; j < 2; j++)
    for (int i = 0; i < 2; i++) {
      const int x = x0 + i, y = y0 + j;
      if (x < 0 || y < 0 || x >= g.w || y >= g.h) continue;
      const float wgt = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
      const float* p = &g.pm[((size_t)y * g.w + x) * 4];
      for (int c = 0; c < 4; c++) out[c] += p[c] * wgt;
    }
}

// Draws glyph g over `dst`, aspect-fitted and centered in the rectangle (cx, cy, maxW, maxH).
void DrawGlyph(Img& dst, const Glyph& g, float cx, float cy, float maxW, float maxH) {
  const float s = std::min(maxW / (g.w - 2), maxH / (g.h - 2));   // the 1 px border does not count toward the size
  const float dw = g.w * s, dh = g.h * s;
  const float x0 = cx - dw / 2, y0 = cy - dh / 2;
  const int ix0 = std::max(0, (int)std::floor(x0)), iy0 = std::max(0, (int)std::floor(y0));
  const int ix1 = std::min(dst.w, (int)std::ceil(x0 + dw)), iy1 = std::min(dst.h, (int)std::ceil(y0 + dh));
  for (int y = iy0; y < iy1; y++)
    for (int x = ix0; x < ix1; x++) {
      float p[4];
      SampleGlyph(g, (x + 0.5f - x0) / s, (y + 0.5f - y0) / s, p);
      if (p[3] <= 0) continue;
      uint8_t* d = &dst.px[((size_t)y * dst.w + x) * 4];
      const float da = d[3] / 255.0f;
      const float oa = p[3] + da * (1 - p[3]);
      for (int c = 0; c < 3; c++) d[c] = (uint8_t)std::clamp((p[c] + d[c] / 255.0f * da * (1 - p[3])) / oa * 255.0f + 0.5f, 0.0f, 255.0f);
      d[3] = (uint8_t)std::clamp(oa * 255.0f + 0.5f, 0.0f, 255.0f);
    }
}

// BC3 (DXT5) to RGBA8.
void DecodeBc3(const uint8_t* blocks, int w, int h, std::vector<uint8_t>& out) {
  out.assign((size_t)w * h * 4, 0);
  const int bw = (w + 3) / 4, bh = (h + 3) / 4;
  for (int by = 0; by < bh; by++)
    for (int bx = 0; bx < bw; bx++) {
      const uint8_t* b = blocks + ((size_t)by * bw + bx) * 16;
      uint8_t al[8]; al[0] = b[0]; al[1] = b[1];
      if (al[0] > al[1]) for (int i = 1; i < 7; i++) al[i + 1] = (uint8_t)(((7 - i) * al[0] + i * al[1]) / 7);
      else { for (int i = 1; i < 5; i++) al[i + 1] = (uint8_t)(((5 - i) * al[0] + i * al[1]) / 5); al[6] = 0; al[7] = 255; }
      uint64_t ab = 0; for (int i = 0; i < 6; i++) ab |= (uint64_t)b[2 + i] << (8 * i);
      const uint16_t c0 = (uint16_t)(b[8] | b[9] << 8), c1 = (uint16_t)(b[10] | b[11] << 8);
      uint8_t col[4][3];
      auto expand = [](uint16_t c, uint8_t* o) { o[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31); o[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63); o[2] = (uint8_t)((c & 31) * 255 / 31); };
      expand(c0, col[0]); expand(c1, col[1]);
      for (int c = 0; c < 3; c++) { col[2][c] = (uint8_t)((2 * col[0][c] + col[1][c]) / 3); col[3][c] = (uint8_t)((col[0][c] + 2 * col[1][c]) / 3); }
      const uint32_t cb = (uint32_t)(b[12] | b[13] << 8 | b[14] << 16 | (uint32_t)b[15] << 24);
      for (int py = 0; py < 4; py++)
        for (int px = 0; px < 4; px++) {
          const int x = bx * 4 + px, y = by * 4 + py;
          if (x >= w || y >= h) continue;
          const int i = py * 4 + px;
          uint8_t* d = &out[((size_t)y * w + x) * 4];
          const uint8_t* c = col[(cb >> (2 * i)) & 3];
          d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = al[(ab >> (3 * i)) & 7];
        }
    }
}

// The original sprite as RGBA8, from the user's own data. False when the format is not BC3 or the data is unreadable.
bool DecodeOriginal(const TextureImpl* t, Img& out) {
  const TextureBuilder& b = t->b;
  const FormatInfo* fi = LookupFormat(b.format);
  const uint8_t* src = GuestData(t);
  if (!fi || !src || fi->bytes != 16 || fi->bw != 4 || fi->bh != 4) return false;
  const SurfaceLayout l = ComputeSurfaceLayout((uint32_t)b.width, (uint32_t)b.height, 1, 1, (uint32_t)std::max(b.levels, 1), fi->bytes, fi->bw, fi->bh);
  if (l.levels.empty()) return false;
  const LevelLayout& l0 = l.levels[0];
  std::vector<uint8_t> lin((size_t)l0.wb * l0.hb * fi->bytes);
  DeswizzleLevel(src + l0.offset, lin.data(), l0, fi->bytes);
  out.w = b.width; out.h = b.height;
  DecodeBc3(lin.data(), out.w, out.h, out.px);
  return true;
}

// Pill: erase the lettered backdrop (the near-white plate or disc in the middle of the chevron) with the body color beside it, then draw the glyph.
void ComposePill(Img& img, const Glyph& g) {
  int x0 = img.w, y0 = img.h, x1 = -1, y1 = -1;
  for (int y = 0; y < img.h; y++)
    for (int x = 0; x < img.w; x++) {
      const uint8_t* p = &img.px[((size_t)y * img.w + x) * 4];
      if (p[3] >= 250 && p[0] >= 205 && p[1] >= 205 && p[2] >= 200 && std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]}) <= 28) {
        x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
      }
    }
  if (x1 < 0 || x1 - x0 < 8 || y1 - y0 < 8) return;
  // The cream backdrop stays: a red or orange button picture (Xbox B, PlayStation Circle) would vanish on the orange chevron without it.
  // The glyph sits inside it with a thin cream rim. The letter that was on the backdrop is covered by the glyph's own opaque art.
  auto cream = [&](int x, int y) {
    const uint8_t* p = &img.px[((size_t)y * img.w + x) * 4];
    return p[3] >= 250 && p[0] >= 205 && p[1] >= 205 && p[2] >= 200 && std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]}) <= 28;
  };
  for (int y = y0; y <= y1; y++) {   // fill the original letter: on each row, everything between the first and last cream pixel becomes cream
    int a = -1, b = -1;
    for (int x = x0; x <= x1; x++) if (cream(x, y)) { if (a < 0) a = x; b = x; }
    if (a < 0) continue;
    const uint8_t* ref = &img.px[((size_t)y * img.w + a) * 4];
    uint8_t col[3] = {ref[0], ref[1], ref[2]};
    for (int x = a; x <= b; x++) if (!cream(x, y)) std::memcpy(&img.px[((size_t)y * img.w + x) * 4], col, 3);
  }
  const float cx = (x0 + x1 + 1) * 0.5f, cy = (y0 + y1 + 1) * 0.5f;
  DrawGlyph(img, g, cx, cy, (x1 - x0 + 1) * 0.84f, (y1 - y0 + 1) * 0.84f);
}

// Plate: our glyph alone, filling a texture that keeps the sprite's shape (longest side 64).
Img ComposePlate(int w, int h, const Glyph& g) {
  Img img;
  const float sc = 64.0f / (float)std::max(w, h);
  img.w = std::max(8, (int)std::lround(w * sc)); img.h = std::max(8, (int)std::lround(h * sc));
  img.px.assign((size_t)img.w * img.h * 4, 0);
  DrawGlyph(img, g, img.w * 0.5f, img.h * 0.5f, (float)img.w, (float)img.h);
  return img;
}

const std::unordered_map<uint64_t, const BtnSprite*>& Table() {
  static const auto m = [] {
    std::unordered_map<uint64_t, const BtnSprite*> t;
    for (const BtnSprite& s : kBtnSprites) t[s.key] = &s;
    return t;
  }();
  return m;
}

TextureImpl* BuildTwin(TextureImpl* t, const BtnSprite& sp, int packed) {   // g_mu held
  LoadAtlas();
  const int row = packed >> 8, gi = packed & 255;
  if (row < 0 || row > 1 || gi < 0 || gi >= kGlyphs || !g_glyph[row][gi].ok()) return nullptr;
  const Glyph& g = g_glyph[row][gi];
  Img img;
  if (sp.mode == 1) {
    if (!DecodeOriginal(t, img)) return nullptr;
    ComposePill(img, g);
  } else {
    img = ComposePlate(t->b.width, t->b.height, g);
  }
  const FormatInfo* fi = LookupFormat(t->b.format);
  auto* buf = new uint8_t[img.px.size()];   // the twin's pixels live as long as the twin (a few KB each)
  std::memcpy(buf, img.px.data(), img.px.size());
  auto* s = new TextureImpl();
  s->host = t->host;
  s->b = TextureBuilder{};
  s->b.device = t->b.device; s->b.target = TT_2D; s->b.width = img.w; s->b.height = img.h; s->b.depth = 1; s->b.levels = 1; s->b.samples = 1;
  s->b.format = (fi && fi->srgb) ? FMT_RGBA8_SRGB : FMT_RGBA8;
  s->b.flags = TF_LINEAR; s->b.stride = img.w * 4; s->b.packaged = buf;
  s->shTried = true; s->biTried = true;
  if (!s->EnsureImage()) { delete s; delete[] buf; return nullptr; }
  return s;
}
}  // namespace

bool BtnIconActive() { return C().trace != nullptr || snap_btnicon_active(); }

void BtnIconForget(TextureImpl* t) {   // DestroyTexture: the twins die with their parent (they are only ever sampled through it)
  if (!t->biAlts) return;
  for (auto& kv : *t->biAlts) if (kv.second) DestroyTexture(kv.second);
  delete t->biAlts;
  t->biAlts = nullptr;
}

TextureImpl* BtnIconSubstitute(TextureImpl* t, const BtnDraw& d) {
  const Cfg& c = C();
  if (t->biTried && !t->biKey) return t;   // not a candidate (the common case)
  std::lock_guard<std::mutex> g(g_mu);
  if (!t->biTried && !t->uploaded) { t->biTried = true; t->biKey = KeyOf(t); }
  if (!t->biKey) {
    if (c.trace && !t->biTried && t->b.width >= 8 && t->b.width <= 512 && t->b.target == TT_2D && !(t->b.flags & (TF_COMPRESSIBLE | TF_DISPLAY | TF_LINEAR_RENDER_TARGET | TF_LINEAR))) {
      static std::set<const TextureImpl*> lateSeen;
      if (lateSeen.insert(t).second) { fprintf(c.trace, "p%d late %dx%d f%x (uploaded before its first UI draw: not keyed)\n", g_presentCount, t->b.width, t->b.height, t->b.format); fflush(c.trace); }
    }
    return t;
  }
  if (c.trace) {
    static std::set<std::tuple<uint64_t, int, int, int, int>> seen;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (d.vbf) { x0 = x1 = d.vbf[0]; y0 = y1 = d.vbf[1]; for (int i = 1; i < 4; i++) { const float x = d.vbf[14 * i], y = d.vbf[14 * i + 1]; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; } }
    if (seen.insert({t->biKey, d.prog, (int)((x1 - x0) / 4), (int)((y1 - y0) / 4), d.rtW}).second) {
      fprintf(c.trace, "p%d key=%016" PRIx64 " %dx%d f%x lv%d prog=%d rt=%dx%d rect=%.0f,%.0f,%.0f,%.0f\n", g_presentCount, t->biKey, t->b.width, t->b.height, t->b.format, t->b.levels, d.prog, d.rtW, d.rtH, x0, y0, x1, y1);
      fflush(c.trace);
    }
  }
  if (!snap_btnicon_active()) return t;
  const auto& tab = Table();
  auto it = tab.find(t->biKey);
  if (it == tab.end()) return t;
  const int packed = snap_btnicon_glyph(it->second->role);
  if (packed < 0) return t;
  if (t->biAlts)
    for (auto& kv : *t->biAlts) if (kv.first == packed) return kv.second ? kv.second : t;
  if (t->biAlts && t->biAlts->size() >= 16) return t;
  TextureImpl* twin = BuildTwin(t, *it->second, packed);
  if (c.trace) { fprintf(c.trace, "p%d twin key=%016" PRIx64 " role=%d mode=%d packed=%d -> %s\n", g_presentCount, t->biKey, it->second->role, it->second->mode, packed, twin ? "built" : "FAILED"); fflush(c.trace); }
  if (!t->biAlts) t->biAlts = new std::vector<std::pair<int, TextureImpl*>>();
  t->biAlts->push_back({packed, twin});   // a failed build is remembered too (null), so it is not retried every draw
  return twin ? twin : t;
}
}  // namespace nvn
