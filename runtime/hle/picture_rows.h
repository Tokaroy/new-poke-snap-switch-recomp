// SPDX-License-Identifier: MIT
// The four picture rows shared by the launcher, the F1 menu and the game's own Settings screen (owner 2026-11-15, after the rival's cleaner split):
//
//   Look  ->  Internal resolution  ->  Anti-aliasing  ->  Performance scaling
//
// They are views over the keys the runtime already reads, so saved settings keep working and nothing new is exported:
//   Internal resolution  gfx_render_scale   (erased = Automatic: the Look and the Anti-aliasing row decide; a number is explicit)
//   Anti-aliasing        gfx_aa             game AA / SMAA / SSAA(+SMAA) / none, or an upscaler at native size (gfx_upscaler = dlaa | fsr | xess)
//   Performance scaling  gfx_upscale_quality  Off = native, else Quality / Balanced / Performance of the upscaler the AA row names
// What the runtime does with them (nvn_aa.cpp AaParseEnv, nvn_entry.cpp ApplyHostEnvPolicy, nvn/upscale/upscale.cpp):
//   * an upscaler (SNAP_UPSCALER) replaces the render scale and the supersampling: the scene is drawn at window height / ratio and the game's FXAA is
//     off unless SNAP_AA is set; sharpening is off unless SNAP_SHARPEN is set. DLAA ignores the quality row (native only).
//   * an explicit supersampling choice (gfx_aa ssaa2...) wins over an explicit render scale; the Enhanced look's own default supersampling steps aside
//     (becomes SMAA) when the render scale is explicit.
// Pure functions (no I/O), so tests/hle/picture_rows_test.cpp can check every combination.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "visual_defaults.h"

namespace snappic {

struct Cfg {                 // the stored values; "" = the key is erased (the default)
  std::string aa, scale, upscaler, quality;
  bool sharpen_set = false;  // gfx_sharpen is stored
};
struct Ctx {                 // the PC this is shown for
  bool enhanced = true;      // the Look
  int window_h = 720;        // window client height in pixels (the monitor's in borderless)
  bool dlss = true, fsr = true, xess = true;   // which upscalers are installed
};

inline bool on(const Cfg& c) { return c.upscaler == "dlss" || c.upscaler == "dlaa" || c.upscaler == "fsr" || c.upscaler == "xess"; }

// ---- the Anti-aliasing row: stored gfx_aa values, plus the three upscaler names
inline std::string aa_row(const Cfg& c) {
  if (c.upscaler == "dlss" || c.upscaler == "dlaa") return "dlaa";
  if (c.upscaler == "fsr") return "fsr";
  if (c.upscaler == "xess") return "xess";
  return c.aa.empty() ? "default" : c.aa;
}
// ---- the Performance scaling row: off | quality | balanced | performance
inline std::string perf_row(const Cfg& c) {
  if (!on(c) || c.upscaler == "dlaa") return "off";
  return c.quality == "quality" || c.quality == "balanced" || c.quality == "performance" ? c.quality : "off";
}
// Pick an Anti-aliasing value. An upscaler value keeps the Performance scaling row; any other value turns the upscaler off (and so Performance scaling).
inline Cfg with_aa(Cfg c, const std::string& v) {
  const std::string perf = perf_row(c);
  if (v == "dlaa" || v == "fsr" || v == "xess") {
    c.aa.clear();
    c.upscaler = v == "dlaa" ? (perf == "off" ? "dlaa" : "dlss") : v;
    c.quality = perf == "off" ? "" : perf;
  } else {
    c.upscaler.clear();
    c.quality.clear();
    c.aa = v == "default" ? "" : v;
  }
  return c;
}
// The upscaler Performance scaling uses when the Anti-aliasing row does not name one yet: DLSS, then FSR, then XeSS, whichever is installed. "" = none.
inline std::string best_backend(const Ctx& x) { return x.dlss ? "dlss" : x.fsr ? "fsr" : x.xess ? "xess" : ""; }
// Pick a Performance scaling value. Anything but Off needs an upscaler: if the Anti-aliasing row is not one, it becomes the best installed one.
inline Cfg with_perf(Cfg c, const std::string& v, const Ctx& x) {
  if (v == "off") {
    c.quality.clear();
    if (c.upscaler == "dlss") c.upscaler = "dlaa";   // DLSS at native size is DLAA
    return c;
  }
  if (!on(c)) {
    const std::string b = best_backend(x);
    if (b.empty()) return c;
    c.upscaler = b;
    c.aa.clear();
  } else if (c.upscaler == "dlaa") c.upscaler = "dlss";
  c.quality = v;
  return c;
}

// ---- what the runtime will draw
inline double ratio_of(const std::string& q) { return q == "quality" ? 1.5 : q == "balanced" ? 1.7241 : q == "performance" ? 2.0 : 1.0; }
inline const char* backend_name(const Cfg& c, bool dlaa_name = true) {
  if (c.upscaler == "dlaa") return dlaa_name ? "DLAA" : "DLSS";
  if (c.upscaler == "dlss") return "DLSS";
  if (c.upscaler == "fsr") return "FSR";
  if (c.upscaler == "xess") return "XeSS";
  return "";
}
inline bool has_token(const std::string& aa, const char* t) { return aa.find(t) != std::string::npos; }
inline float out_scale(const Ctx& x) { return std::clamp((float)x.window_h / 1080.0f, 0.5f, 4.0f); }
inline float explicit_scale(const Cfg& c) { const float v = (float)std::atof(c.scale.c_str()); return (v >= 0.25f && v <= 8.f) ? v : 0.f; }

struct Eff {
  float scene = 1.0f;        // the 3D picture's scale over 1920 x 1080
  bool ssaa = false;         // supersampling is what set it
  bool upscaled = false;
  float output = 1.0f;       // upscaler output scale
  std::string aa;            // the AA step in effect: "FXAA" "SMAA" "None" or "" (an upscaler)
};
inline Eff effective(const Cfg& c, const Ctx& x) {
  Eff e;
  if (on(c)) {
    e.upscaled = true;
    e.output = out_scale(x);
    e.scene = (float)(e.output / (c.upscaler == "dlaa" ? 1.0 : ratio_of(perf_row(c))));
    e.aa = has_token(c.aa, "smaa") ? "SMAA" : "";
    return e;
  }
  const bool user = !c.aa.empty();
  std::string aa = user ? c.aa : (x.enhanced ? "ssaa2,smaa" : "off");
  if (!user && explicit_scale(c) > 0.f && has_token(aa, "ssaa2")) aa = "smaa";   // an explicit render scale wins over the Look's default supersampling
  e.aa = has_token(aa, "smaa") ? "SMAA" : (aa == "none" ? "None" : "FXAA");
  e.scene = explicit_scale(c) > 0.f ? explicit_scale(c) : 1.0f;
  if (has_token(aa, "ssaa2")) {
    e.ssaa = true;
    e.scene = user ? std::clamp(2.0f * (float)x.window_h / 1080.0f, 1.0f, 3.0f)
                   : snapdef::ssaa_auto_scale(x.window_h, 2160.0f, 3.0f);
  }
  return e;
}
inline std::string dims(float s) {
  char b[48];
  std::snprintf(b, sizeof b, "%d x %d", (int)std::lround(1920 * s), (int)std::lround(1080 * s));
  return b;
}

// ---- one line per row: which value is in effect when another row overrides it ("" = nothing overrides this row)
inline std::string note_resolution(const Cfg& c, const Ctx& x) {
  const Eff e = effective(c, x);
  if (e.upscaled && explicit_scale(c) > 0.f) return std::string("Not used: ") + backend_name(c) + " draws " + dims(e.scene) + ", from the window size";
  if (e.upscaled) return "";
  if (e.ssaa && explicit_scale(c) <= 0.f && c.aa.empty()) return "Automatic: 2x supersampling, drawn at " + dims(e.scene);
  if (e.ssaa && !c.aa.empty() && explicit_scale(c) > 0.f && std::fabs(explicit_scale(c) - e.scene) > 0.005f)
    return "Not used: supersampling draws " + dims(e.scene);
  return "";
}
inline std::string note_aa(const Cfg& c, const Ctx& x) {
  const Eff e = effective(c, x);
  if (on(c)) {
    if (!c.aa.empty()) return has_token(c.aa, "smaa") ? "Also on: SMAA from an older setting" : "Ignored: " + c.aa + " (an upscaler is on)";
    return "";
  }
  if (c.aa.empty() && x.enhanced && explicit_scale(c) > 0.f) return "In effect: SMAA. Supersampling steps aside for a set resolution";
  return "";
}
// The SMAA quality row (Low, Medium, High, Ultra; runtime/nvn/nvn_aa.cpp SmaaPreset): only SMAA reads it.
inline std::string note_smaa(const Cfg& c, const Ctx& x) {
  const Eff e = effective(c, x);
  if (e.aa == "SMAA") return "";
  if (e.upscaled) return "Not used: an upscaler is on";
  if (e.aa == "None") return "Not used: anti-aliasing is off";
  return "Not used: the game's own FXAA is on";
}
inline std::string note_perf(const Cfg& c, const Ctx& x) {
  if (!on(c)) return "";
  const Eff e = effective(c, x);
  const std::string p = perf_row(c);
  if (p == "off") return std::string(backend_name(c)) + " at native size: draws " + dims(e.scene);
  return std::string(backend_name(c, false)) + " draws " + dims(e.scene) + ", shown at " + dims(e.output);
}
inline std::string note_sharpen(const Cfg& c) {
  return on(c) && !c.sharpen_set ? "In effect: Off. An upscaler skips sharpening unless you set it" : "";
}
// The Automatic label of the Internal resolution row: what it comes to on this PC.
inline std::string auto_label(const Cfg& c, const Ctx& x) {
  Cfg a = c;
  a.scale.clear();
  const Eff e = effective(a, x);
  if (e.upscaled) return "Automatic";
  return "Automatic (" + dims(e.scene) + ")";
}

}  // namespace snappic
