// SPDX-License-Identifier: MIT
// HUD scale (SNAP_HUD_SCALE = auto (100%) | 100 | 125 | 150 | 175 | 200). Design, limits and results: docs/hud-scale.md.
//
// The game draws its interface (HUD, menus, dialogue) straight into the 1920x1080 window texture as many small sprite and text draws, each with
// its own positions in guest pixels. A HUD scale s > 1 enlarges those draws around the screen center by changing the viewport (and scissor) a
// draw is emitted with: guest x' = p x + q, y' = r y + t. The 3D view, post passes, full-screen fills, photos and every draw that is not an
// interface draw into the window texture keep the identity.
//
// Scaling each draw on its own would pull a button apart from its label, so draws are grouped: boxes that overlap or lie within 40 px of each other
// (a button, its icon and its label; a film counter and its frame) form a cluster, built at each present from the draws of that frame and used by the next
// frame (the interface hardly changes between two presents). A draw next to a cluster (within 40 px) joins it, so text that gains a glyph a frame keeps the
// transform of the line it grows. A cluster scales uniformly by s, at most as far as it still fits the screen with a margin of 3% (so a panel that fills
// the screen stays 100%). It scales about the point that keeps it where the 100% layout puts it: the edge it sits nearest to when its center is in the outer
// third of the screen on an axis, else its center; a cluster that is still there next frame keeps that point. It then moves back inside if its scaled box
// would cross a screen edge, keeping up to 3% (at most its original distance) from that edge. Everything inside one cluster moves together.
// Touch input: snap_window_to_guest maps a window position to guest pixels through the last present's blit rectangle and then undoes the cluster
// transform that drew the frame on screen, so a tap lands on the element it was drawn on at every scale (and on ultrawide windows).
#include "nvn_internal.h"
#include "../hle/input_core.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace nvn {
namespace {
constexpr float kW = 1920.0f, kH = 1080.0f;
constexpr float kMargin = 0.03f;     // screen fraction a scaled cluster may come up to an edge
constexpr float kGap = 40.0f;        // boxes closer than this (guest px) belong to one cluster

struct Box { float x0, y0, x1, y1; };
struct Used { Box orig; Box shown; HudXf xf; };
struct Cl { Box b; HudXf xf; };                          // a cluster of the last frame and the transform every draw inside it gets
struct Rec { Box b; float ax, ay; };                     // a cluster's box and the point it scales about, kept to the next frame
std::mutex g_mu;
std::string g_setting = "auto";      // as chosen: auto or a percent
std::atomic<float> g_eff{1.0f};      // scale in effect now (>= 1)
int g_winH = 0;
std::vector<Box> g_cur;              // boxes of this frame's scaled-class draws
std::vector<Cl> g_prev;              // clusters made from the last frame: they drive this frame
std::vector<Rec> g_recs;             // the same clusters' anchors: a cluster that is still there next frame scales about the same point
std::vector<Used> g_curUsed;         // what this frame drew with
std::vector<Used> g_shown;           // what the frame on screen was drawn with (touch input)
bool g_inited = false;
int g_log = 0;

struct Geom { int ew = 0, eh = 0, dx = 0, dy = 0, dw = 0, dh = 0, sx = 0, sy = 0, sw = 0, sh = 0; float rs = 1.0f, xoff = 0.0f; bool ok = false; } g_geom;

// Auto means 100% (no scaling) until the placement is right at every size: the owner's 4K screen showed off-center, oversized panels with the old
// height steps (<1260 1.0, <1800 1.25, <2500 1.5, <3600 1.75, else 2.0). A size is only used when it is chosen.
float AutoFor(int) { return 1.0f; }

// "auto", "100".."200", or a factor ("1.5")
float ParseSetting(const std::string& s, bool* isAuto) {
  *isAuto = false;
  if (s.empty() || s == "auto" || s == "default") { *isAuto = true; return 1.0f; }
  float v = (float)atof(s.c_str());
  if (v >= 10.0f) v /= 100.0f;
  if (!(v >= 1.0f)) v = 1.0f;
  return std::min(v, 2.0f);
}
void Recompute() {
  bool a; const float v = ParseSetting(g_setting, &a);
  g_eff.store(a ? AutoFor(g_winH) : v, std::memory_order_relaxed);
}
void Init() {
  if (g_inited) return;
  g_inited = true;
  if (const char* e = getenv("SNAP_HUD_SCALE")) if (*e) g_setting = e;
  const char* l = getenv("SNAP_HUD_LOG"); g_log = l ? std::max(1, atoi(l)) : 0;
  Recompute();
  hle::input::g_window_to_guest = &HudWindowToGuest;   // mouse touch follows the blit rectangle and the HUD transform
}
float Area(const Box& b) { return (b.x1 - b.x0) * (b.y1 - b.y0); }
}  // namespace
static bool MapLocked(float nx, float ny, float* gx, float* gy);

// the point a new box scales about, so it stays where the 100% layout puts it: its own center, or the edge it sits nearest to when its center is in the
// outer third of the screen on that axis (a corner icon or a left-hand panel grows away from its edge, a centered dialogue box around its middle)
static void AnchorFor(const Box& c, float* ax, float* ay) {
  const float cx = (c.x0 + c.x1) * 0.5f, cy = (c.y0 + c.y1) * 0.5f;
  *ax = cx < kW * 0.33f ? c.x0 : cx > kW * 0.67f ? c.x1 : cx;
  *ay = cy < kH * 0.33f ? c.y0 : cy > kH * 0.67f ? c.y1 : cy;
}

// the transform for a cluster/box c, scaling by at most s about (ax, ay): the placement the 100% layout gives it stays (a panel grows where it sits),
// then it is moved back inside if it would cross a screen edge, keeping up to 3% (at most its original distance) from that edge
static HudXf MakeXf(const Box& c, float ax, float ay, float s) {
  HudXf o;
  const float cw = c.x1 - c.x0, ch = c.y1 - c.y0;
  const float mlx = std::min(std::max(c.x0, 0.0f), kMargin * kW), mrx = std::min(std::max(kW - c.x1, 0.0f), kMargin * kW);
  const float mty = std::min(std::max(c.y0, 0.0f), kMargin * kH), mby = std::min(std::max(kH - c.y1, 0.0f), kMargin * kH);
  const float sc = std::min({s, (kW - mlx - mrx) / cw, (kH - mty - mby) / ch});
  if (!(sc > 1.001f)) return o;
  const float nx0 = ax + (c.x0 - ax) * sc, nx1 = ax + (c.x1 - ax) * sc, ny0 = ay + (c.y0 - ay) * sc, ny1 = ay + (c.y1 - ay) * sc;
  float dx = 0.0f, dy = 0.0f;
  if (nx0 < mlx) dx = mlx - nx0; else if (nx1 > kW - mrx) dx = (kW - mrx) - nx1;
  if (ny0 < mty) dy = mty - ny0; else if (ny1 > kH - mby) dy = (kH - mby) - ny1;
  o.on = true; o.p = sc; o.q = ax * (1.0f - sc) + dx; o.r = sc; o.t = ay * (1.0f - sc) + dy;
  return o;
}

float HudScaleNow() { if (!g_inited) { std::lock_guard<std::mutex> lk(g_mu); Init(); } return g_eff.load(std::memory_order_relaxed); }

bool HudOnDraw(const float* box, HudXf* out) {
  *out = HudXf{};
  const float s = HudScaleNow();
  if (s <= 1.001f || !box) return false;
  Box b{box[0], box[1], box[2], box[3]};
  const float w = b.x1 - b.x0, h = b.y1 - b.y0;
  if (!(w > 0 && h > 0) || std::max(w, h) < 16.0f) return false;                       // model-space or NDC positions, not pixels
  if (b.x0 < -2 || b.y0 < -2 || b.x1 > kW + 2 || b.y1 > kH + 2) return false;       // moving in from outside the screen: unscaled
  if (w >= 0.9f * kW && h >= 0.9f * kH) return false;                                 // backdrops, fades, wipes, full-screen layers
  std::lock_guard<std::mutex> lk(g_mu);
  g_cur.push_back(b);
  const float cx = (b.x0 + b.x1) * 0.5f, cy = (b.y0 + b.y1) * 0.5f;
  const Cl* hit = nullptr;                                                            // its cluster from the last frame: the smallest one it lies in
  float best = 1e30f;
  for (const Cl& k : g_prev)
    if (cx >= k.b.x0 && cx <= k.b.x1 && cy >= k.b.y0 && cy <= k.b.y1 && Area(k.b) < best) { best = Area(k.b); hit = &k; }
  if (!hit) {   // text that grows by a glyph a frame: a draw next to a cluster of the last frame (within kGap) belongs to it and keeps its transform
    float bd = 1e30f;
    for (const Cl& k : g_prev) {
      const float gx = std::max(0.0f, std::max(k.b.x0 - b.x1, b.x0 - k.b.x1)), gy = std::max(0.0f, std::max(k.b.y0 - b.y1, b.y0 - k.b.y1));
      if (gx <= kGap && gy <= kGap && gx + gy < bd) { bd = gx + gy; hit = &k; }
    }
  }
  float ax, ay;
  AnchorFor(b, &ax, &ay);
  const HudXf xf = hit ? hit->xf : MakeXf(b, ax, ay, s);                              // else the draw alone
  if (!xf.on) return false;
  *out = xf;
  g_curUsed.push_back({b, Box{out->p * b.x0 + out->q, out->r * b.y0 + out->t, out->p * b.x1 + out->q, out->r * b.y1 + out->t}, *out});
  return true;
}

// once per present, after the frame's draws: build the clusters for the next frame and keep this frame's transforms for touch input
void HudOnPresent(int winW, int winH, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh, const TextureImpl* src) {
  std::lock_guard<std::mutex> lk(g_mu);
  Init();
  if (winH != g_winH) { g_winH = winH; Recompute(); if (g_log) fprintf(stderr, "[hud] window height %d: scale %.2f (%s)\n", winH, g_eff.load(), g_setting.c_str()); }
  if (src && src->b.width > 0 && src->b.height > 0) {
    g_geom.ew = winW; g_geom.eh = winH; g_geom.dx = dx; g_geom.dy = dy; g_geom.dw = std::max(1, dw); g_geom.dh = std::max(1, dh);
    g_geom.sx = sx; g_geom.sy = sy; g_geom.sw = sw; g_geom.sh = sh;
    g_geom.rs = (float)src->HH() / (float)src->b.height;
    g_geom.xoff = src->wide > 1.0f ? ((float)src->HW() - (float)src->b.width * g_geom.rs) * 0.5f : 0.0f;
    g_geom.ok = true;
  }
  g_shown.swap(g_curUsed);
  g_curUsed.clear();
  if (g_log >= 3 && g_geom.ok && !g_shown.empty()) {   // touch self-check: each shown box's center, put back through the window mapping, must give the original center
    static int m = 0;
    if ((m++ % 240) == 0) {
      int bad = 0, n = 0;
      for (const Used& e : g_shown) {
        if (n >= 64) break;
        n++;
        const float cx = (e.shown.x0 + e.shown.x1) * 0.5f, cy = (e.shown.y0 + e.shown.y1) * 0.5f;
        const float wx = ((float)g_geom.dx + (((cx * g_geom.rs + g_geom.xoff) - (float)g_geom.sx) / (float)g_geom.sw) * (float)g_geom.dw) / (float)g_geom.ew;
        const float wy = ((float)g_geom.dy + (((cy * g_geom.rs) - (float)g_geom.sy) / (float)g_geom.sh) * (float)g_geom.dh) / (float)g_geom.eh;
        float gx = 0, gy = 0;
        MapLocked(wx, wy, &gx, &gy);
        const float ox = (e.orig.x0 + e.orig.x1) * 0.5f, oy = (e.orig.y0 + e.orig.y1) * 0.5f;
        if (std::fabs(gx - ox) > 1.5f || std::fabs(gy - oy) > 1.5f) bad++;   // a smaller shown box on top of this one wins the lookup: expected for stacked draws
      }
      fprintf(stderr, "[hud] touch check: %d of %d shown boxes map back to their original center (scale %.2f, window %dx%d)\n", n - bad, n, g_eff.load(), g_geom.ew, g_geom.eh);
    }
  }
  // clusters: boxes that overlap or lie within kGap of each other, merged to a fixed point (sweep over x)
  std::vector<Box> cl;
  if (g_eff.load(std::memory_order_relaxed) > 1.001f && !g_cur.empty()) {
    std::vector<Box> v = g_cur;
    std::sort(v.begin(), v.end(), [](const Box& a, const Box& b) { return a.x0 < b.x0; });
    std::vector<char> dead;
    bool merged = true;
    while (merged && v.size() > 1) {
      merged = false;
      dead.assign(v.size(), 0);
      for (size_t i = 0; i < v.size(); i++) {
        if (dead[i]) continue;
        for (size_t j = i + 1; j < v.size(); j++) {
          if (dead[j]) continue;
          if (v[j].x0 > v[i].x1 + kGap) break;
          if (v[j].y0 > v[i].y1 + kGap || v[j].y1 < v[i].y0 - kGap) continue;
          v[i] = Box{std::min(v[i].x0, v[j].x0), std::min(v[i].y0, v[j].y0), std::max(v[i].x1, v[j].x1), std::max(v[i].y1, v[j].y1)};
          dead[j] = 1; merged = true;
        }
      }
      std::vector<Box> nv;
      for (size_t i = 0; i < v.size(); i++) if (!dead[i]) nv.push_back(v[i]);
      v.swap(nv);
      std::sort(v.begin(), v.end(), [](const Box& a, const Box& b) { return a.x0 < b.x0; });
    }
    cl.swap(v);
  }
  if (g_log >= 2 && !cl.empty()) {
    static int n = 0;
    if ((n++ % 120) == 0) {
      fprintf(stderr, "[hud] present: %zu draws -> %zu clusters (scale %.2f)\n", g_cur.size(), cl.size(), g_eff.load());
      for (size_t i = 0; i < cl.size() && i < 24; i++) fprintf(stderr, "[hud]   cluster %.0f,%.0f..%.0f,%.0f\n", cl[i].x0, cl[i].y0, cl[i].x1, cl[i].y1);
    }
  }
  {   // each cluster's transform for the next frame. A cluster that overlaps one of the last frame keeps its anchor (the point it scales about), so text
      // that gains a glyph a frame, or a panel that grows, does not creep; a new cluster scales about its own center.
    const float sNow = g_eff.load(std::memory_order_relaxed);
    std::vector<Cl> np; std::vector<Rec> nr;
    std::vector<char> used(g_recs.size(), 0);
    for (const Box& c : cl) {
      float ax, ay, bo = 0.0f;
      AnchorFor(c, &ax, &ay);
      int bi = -1;
      for (size_t i = 0; i < g_recs.size(); i++) {
        if (used[i]) continue;
        const Box& o = g_recs[i].b;
        const float ow = std::min(c.x1, o.x1) - std::max(c.x0, o.x0), oh = std::min(c.y1, o.y1) - std::max(c.y0, o.y0);
        if (ow > 0 && oh > 0 && ow * oh > bo) { bo = ow * oh; bi = (int)i; }
      }
      if (bi >= 0) {
        const Rec& r = g_recs[(size_t)bi];
        if (r.ax >= c.x0 && r.ax <= c.x1 && r.ay >= c.y0 && r.ay <= c.y1) { ax = r.ax; ay = r.ay; used[(size_t)bi] = 1; }
      }
      np.push_back(Cl{c, MakeXf(c, ax, ay, sNow)});
      nr.push_back(Rec{c, ax, ay});
    }
    g_prev.swap(np);
    g_recs.swap(nr);
  }
  g_cur.clear();
}

// window position (0..1 over the client area) -> guest pixels of the original interface layout. false: no geometry yet. g_mu held.
static bool MapLocked(float nx, float ny, float* gx, float* gy) {
  if (!g_geom.ok || g_geom.ew <= 0 || g_geom.eh <= 0) return false;
  const float u = std::clamp((nx * (float)g_geom.ew - (float)g_geom.dx) / (float)g_geom.dw, 0.0f, 1.0f);
  const float v = std::clamp((ny * (float)g_geom.eh - (float)g_geom.dy) / (float)g_geom.dh, 0.0f, 1.0f);
  float x = ((float)g_geom.sx + u * (float)g_geom.sw - g_geom.xoff) / g_geom.rs;
  float y = ((float)g_geom.sy + v * (float)g_geom.sh) / g_geom.rs;
  const Used* best = nullptr; float ba = 1e30f;
  for (const Used& e : g_shown)
    if (x >= e.shown.x0 && x <= e.shown.x1 && y >= e.shown.y0 && y <= e.shown.y1 && Area(e.shown) < ba) { ba = Area(e.shown); best = &e; }
  if (best) { x = (x - best->xf.q) / best->xf.p; y = (y - best->xf.t) / best->xf.r; }
  *gx = x; *gy = y;
  return true;
}
bool HudWindowToGuest(float nx, float ny, float* gx, float* gy) {
  std::lock_guard<std::mutex> lk(g_mu);
  return MapLocked(nx, ny, gx, gy);
}
}  // namespace nvn

// live: "auto", "100".."200" (the options menus); the next frame uses it
extern "C" void snap_hud_scale_set(const char* v) {
  std::lock_guard<std::mutex> lk(nvn::g_mu);
  nvn::Init();
  nvn::g_setting = (v && *v) ? v : "auto";
  nvn::Recompute();
}
extern "C" const char* snap_hud_scale_get() {
  static thread_local std::string s;
  std::lock_guard<std::mutex> lk(nvn::g_mu);
  nvn::Init();
  s = nvn::g_setting;
  return s.c_str();
}
extern "C" float snap_hud_scale_effective() { return nvn::HudScaleNow(); }
