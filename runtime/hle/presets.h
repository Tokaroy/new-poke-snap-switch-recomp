// SPDX-License-Identifier: MIT
// Graphics presets: one row (Original / Balanced / Enhanced / Max, or Custom) shared by the launcher, the F1 menu and the game's own Settings screen (docs/presets.md).
//
// A preset is a set of stored settings values (the same keys the rows below it use), so nothing new is exported to the runtime. The row is not stored: it is worked
// out from the rows, so changing any single row afterwards shows Custom, and putting the rows back to a preset's values shows that preset again.
// "" in the table means the key is erased (the row's own default). Pure functions with get/put callbacks, so tests/hle/picture_rows_test.cpp can check every
// preset and the launcher and the F1 menu use one table.
//
//   Original    the game as it shipped: Look Original (its own FXAA, no extras), nothing added.
//   Balanced    the Enhanced look made lighter: SMAA (or the best installed upscaler at Quality), 8x filtering, medium SMAA, nothing extra.
//   Enhanced    what a new install runs: every row at its default under the Enhanced look (supersampling sized to the screen with SMAA, sharpening, 16x filtering...).
//   Max         everything committed turned up: DLAA when DLSS is installed (else 2x supersampling + SMAA), the Maximum lighting preset, Ultra SMAA and occlusion,
//               leaf and particle upgrades, light bokeh, the atmosphere effects (light shafts, height fog, auto exposure, soft glow), the water effects (reflections,
//               caustics, shoreline foam), bounce light, 120 fps.
// Left out on purpose: draw distance and full-rate animation (photo scores unverified), frame generation, motion blur, HDR and color, the frame rate limit, vsync.
// Effects that are not committed yet join Max when they are (docs/presets.md lists them).
#pragma once
#include <cmath>
#include <cstdlib>
#include <functional>
#include <string>

#include "picture_rows.h"

namespace snappreset {

constexpr int kCount = 4;
inline const char* id(int p) { static const char* const k[kCount] = {"original", "balanced", "enhanced", "max"}; return p >= 0 && p < kCount ? k[p] : ""; }
inline const char* label(int p) { static const char* const k[kCount] = {"Original", "Balanced", "Enhanced", "Max"}; return p >= 0 && p < kCount ? k[p] : "Custom"; }
inline int from_id(const std::string& s) { for (int p = 0; p < kCount; p++) if (s == id(p)) return p; return -1; }

struct Row { const char* key; const char* v[kCount]; };   // Original, Balanced, Enhanced, Max; "" = erased
// gfx_look comes first: the F1 menu and the launcher clear the Look's own keys when it is set, then the rows below are written.
inline const Row* rows(int* n) {
  static const Row t[] = {
      {"gfx_look", {"original", "", "", ""}},
      {"gfx_render_scale", {"", "", "", ""}},
      {"gfx_sharpen", {"", "", "", ""}},
      {"gfx_aniso", {"", "8", "", "16"}},
      {"gfx_ui_font", {"", "", "", ""}},
      {"photo_export", {"", "", "", ""}},
      {"hd_photos", {"", "", "", ""}},
      {"gfx_smaa_quality", {"", "medium", "", "ultra"}},
      {"gfx_lighting", {"", "", "", "maximum"}},
      {"fps_target", {"", "", "", "120"}},
      {"foliage_aa", {"", "", "", "1"}},
      {"leaf_translucency", {"", "", "", "0.5"}},
      {"soft_particles", {"", "", "", "1"}},
      {"gfx_bokeh_dof", {"", "", "", "0.6"}},
      {"ssao_quality", {"", "", "", "ultra"}},
      {"atmos_shafts", {"", "", "", "1"}},
      {"atmos_fog", {"", "", "", "1"}},
      {"atmos_exposure", {"", "", "", "1"}},
      {"atmos_bloom", {"", "", "", "1"}},
      {"water_ssr", {"", "", "", "1"}},
      {"water_caustics", {"", "", "", "1"}},
      {"water_foam", {"", "", "", "1"}},
      {"gfx_bounce", {"", "", "", "1"}},
  };
  *n = (int)(sizeof t / sizeof t[0]);
  return t;
}

// The picture triple (gfx_aa, gfx_upscaler, gfx_upscale_quality) of a preset on a PC with these upscalers installed.
inline snappic::Cfg picture_for(int p, const snappic::Ctx& x) {
  snappic::Cfg c;
  if (p == 1) {                                   // Balanced: the best installed upscaler at Quality, else SMAA
    if (!snappic::best_backend(x).empty()) return snappic::with_perf(c, "quality", x);
    c.aa = "smaa";
  } else if (p == 3) {                            // Max: DLAA when DLSS is installed, else 2x supersampling + SMAA
    if (x.dlss) return snappic::with_aa(c, "dlaa");
    c.aa = "ssaa2,smaa";
  }
  return c;
}

using Get = std::function<std::string(const std::string&)>;                     // the stored value, "" when the key is erased
using Put = std::function<void(const std::string&, const std::string&)>;       // set a key; "" erases it

// A stored value as the table writes it: the row defaults ("default", "auto", "off" upscaler, "native") are the erased key.
inline std::string norm(const std::string& key, const std::string& v) {
  if (v == "default") return "";
  if (key == "gfx_render_scale" && v == "auto") return "";
  if (key == "gfx_smaa_quality" && v == "high") return "";
  if (key == "gfx_upscaler" && v == "off") return "";
  if (key == "gfx_upscale_quality" && v == "native") return "";
  return v;
}
inline bool numeric(const std::string& s) { char* e = nullptr; if (s.empty()) return false; std::strtod(s.c_str(), &e); return e && !*e; }
inline bool same(const std::string& a, const std::string& b) {
  if (a == b) return true;
  return numeric(a) && numeric(b) && std::fabs(std::atof(a.c_str()) - std::atof(b.c_str())) < 1e-4;
}

// Write a preset: gfx_look first, then every other row, then the picture triple.
inline void apply(int p, const Put& put, const snappic::Ctx& x) {
  if (p < 0 || p >= kCount) return;
  int n = 0;
  const Row* t = rows(&n);
  for (int i = 0; i < n; i++) put(t[i].key, t[i].v[p]);
  const snappic::Cfg c = picture_for(p, x);
  put("gfx_aa", c.aa);
  put("gfx_upscaler", c.upscaler);
  put("gfx_upscale_quality", c.quality);
}

// Which preset the stored rows add up to: its index, or -1 for Custom. The picture triple may be the one of any PC (a preset written before DLSS was installed
// still shows as that preset afterwards).
inline int detect(const Get& get) {
  int n = 0;
  const Row* t = rows(&n);
  for (int p = 0; p < kCount; p++) {
    bool all = true;
    for (int i = 0; i < n && all; i++) all = same(norm(t[i].key, get(t[i].key)), t[i].v[p]);
    if (!all) continue;
    const std::string a = norm("gfx_aa", get("gfx_aa")), u = norm("gfx_upscaler", get("gfx_upscaler")), q = norm("gfx_upscale_quality", get("gfx_upscale_quality"));
    for (int m = 0; m < 8; m++) {
      snappic::Ctx x;
      x.dlss = m & 1; x.fsr = m & 2; x.xess = m & 4;
      const snappic::Cfg c = picture_for(p, x);
      if (same(a, c.aa) && u == c.upscaler && q == c.quality) return p;
    }
  }
  return -1;
}
inline std::string current(const Get& get) { const int p = detect(get); return p < 0 ? "custom" : id(p); }

}  // namespace snappreset
