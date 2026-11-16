// SPDX-License-Identifier: MIT
// Unit test for runtime/hle/picture_rows.h: the Internal resolution / Anti-aliasing / Performance scaling rows over the stored keys.
// Build and run with tests\hle\run_picture_rows_test.ps1.
#include <cstdio>
#include <map>
#include <string>

#include "../../runtime/hle/picture_rows.h"
#include "../../runtime/hle/presets.h"

static int g_fail = 0;
#define EXPECT(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
using namespace snappic;

int main() {
  Ctx x;   // Enhanced, 720p window, all upscalers installed
  x.window_h = 1080;

  // reading saved settings (old keys keep working)
  { Cfg c; EXPECT(aa_row(c) == "default" && perf_row(c) == "off"); }
  { Cfg c; c.aa = "smaa"; EXPECT(aa_row(c) == "smaa"); }
  { Cfg c; c.upscaler = "dlaa"; EXPECT(aa_row(c) == "dlaa" && perf_row(c) == "off"); }
  { Cfg c; c.upscaler = "dlss"; c.quality = "quality"; EXPECT(aa_row(c) == "dlaa" && perf_row(c) == "quality"); }
  { Cfg c; c.upscaler = "dlss"; EXPECT(perf_row(c) == "off"); }                                   // DLSS at native size is DLAA
  { Cfg c; c.upscaler = "dlaa"; c.quality = "performance"; EXPECT(perf_row(c) == "off"); }        // DLAA ignores the quality key
  { Cfg c; c.upscaler = "fsr"; c.quality = "balanced"; EXPECT(aa_row(c) == "fsr" && perf_row(c) == "balanced"); }
  { Cfg c; c.quality = "performance"; EXPECT(perf_row(c) == "off"); }                              // quality without an upscaler does nothing

  // every Anti-aliasing value, then every Performance scaling value: the rows read back what was picked
  const char* aas[] = {"default", "off", "smaa", "ssaa2", "ssaa2,smaa", "none", "dlaa", "fsr", "xess"};
  const char* perfs[] = {"off", "quality", "balanced", "performance"};
  for (const char* a : aas) for (const char* p : perfs) {
    Cfg c = with_aa(Cfg{}, a);
    EXPECT(aa_row(c) == a);
    c = with_perf(c, p, x);
    const bool up = std::string(a) == "dlaa" || std::string(a) == "fsr" || std::string(a) == "xess";
    if (std::string(p) == "off") { EXPECT(perf_row(c) == "off"); EXPECT(aa_row(c) == a); }
    else {
      EXPECT(perf_row(c) == p);
      EXPECT(aa_row(c) == (up ? a : "dlaa"));            // a non-upscaler AA gives way to the best installed upscaler (DLSS here)
      EXPECT(c.aa.empty() && c.upscaler == (std::string(a) == "fsr" ? "fsr" : std::string(a) == "xess" ? "xess" : "dlss"));
    }
    // picking a non-upscaler AA again clears the upscaler and the quality
    Cfg d = with_aa(c, "smaa");
    EXPECT(!on(d) && d.quality.empty() && perf_row(d) == "off" && d.aa == "smaa");
  }
  { Cfg c = with_aa(Cfg{}, "dlaa"); EXPECT(c.upscaler == "dlaa" && c.quality.empty()); c = with_perf(c, "quality", x); EXPECT(c.upscaler == "dlss" && c.quality == "quality");
    c = with_perf(c, "off", x); EXPECT(c.upscaler == "dlaa" && c.quality.empty()); }
  { Ctx n = x; n.dlss = false; EXPECT(best_backend(n) == "fsr"); n.fsr = false; EXPECT(best_backend(n) == "xess"); n.xess = false; EXPECT(best_backend(n).empty());
    Cfg c = with_perf(Cfg{}, "quality", n); EXPECT(!on(c) && c.quality.empty()); }   // nothing installed: no change

  // what the runtime draws (mirrors nvn_aa.cpp / nvn_entry.cpp / upscale.cpp)
  { Cfg c; Eff e = effective(c, x); EXPECT(e.ssaa && std::fabs(e.scene - 2.0f) < 1e-4 && e.aa == "SMAA"); }          // Enhanced default at 1080p: 2x supersampling
  { Cfg c; Ctx w = x; w.window_h = 2160; Eff e = effective(c, w); EXPECT(std::fabs(e.scene - 2.0f) < 1e-4); }       // 4K window: the 2160-line budget
  { Cfg c; Ctx o = x; o.enhanced = false; Eff e = effective(c, o); EXPECT(!e.ssaa && e.scene == 1.0f && e.aa == "FXAA"); }
  { Cfg c; c.scale = "1.00"; Eff e = effective(c, x); EXPECT(!e.ssaa && e.scene == 1.0f && e.aa == "SMAA"); EXPECT(!note_aa(c, x).empty()); }
  { Cfg c; c.scale = "1.50"; EXPECT(note_resolution(c, x).empty() && effective(c, x).scene == 1.5f); }
  { Cfg c; c.aa = "ssaa2"; c.scale = "4.00"; Eff e = effective(c, x); EXPECT(e.ssaa && std::fabs(e.scene - 2.0f) < 1e-4); EXPECT(note_resolution(c, x).find("Not used") == 0); }
  { Cfg c; c.aa = "ssaa2"; c.scale = "2.00"; EXPECT(note_resolution(c, x).empty()); }                                  // same number: nothing is overridden
  { Cfg c = with_aa(Cfg{}, "dlaa"); c.scale = "2.00"; Eff e = effective(c, x); EXPECT(e.upscaled && e.scene == 1.0f);   // DLAA plus a render scale above 1
    EXPECT(note_resolution(c, x).find("Not used: DLAA") == 0); }
  { Cfg c = with_perf(with_aa(Cfg{}, "dlaa"), "performance", x); Ctx w = x; w.window_h = 2160; Eff e = effective(c, w);
    EXPECT(std::fabs(e.scene - 1.0f) < 1e-4 && std::fabs(e.output - 2.0f) < 1e-4); EXPECT(note_perf(c, w).find("DLSS draws 1920 x 1080") == 0); }
  { Cfg c = with_perf(with_aa(Cfg{}, "fsr"), "quality", x); Ctx w = x; w.window_h = 2160; EXPECT(dims(effective(c, w).scene) == "2560 x 1440"); }
  { Cfg c; c.upscaler = "dlaa"; c.aa = "ssaa2"; EXPECT(note_aa(c, x).find("Ignored") == 0); EXPECT(!effective(c, x).ssaa); }  // an upscaler plus supersampling (older save)
  { Cfg c; c.upscaler = "dlaa"; c.aa = "smaa"; EXPECT(note_aa(c, x).find("Also on") == 0); }
  { Cfg c; c.upscaler = "dlaa"; EXPECT(!note_sharpen(c).empty()); c.sharpen_set = true; EXPECT(note_sharpen(c).empty()); EXPECT(note_sharpen(Cfg{}).empty()); }
  { Cfg c; EXPECT(auto_label(c, x) == "Automatic (3840 x 2160)"); EXPECT(note_resolution(c, x).find("Automatic: 2x supersampling") == 0); Ctx o = x; o.enhanced = false; EXPECT(auto_label(c, o) == "Automatic (1920 x 1080)"); }
  { Cfg c; EXPECT(note_smaa(c, x).empty()); c.aa = "smaa"; EXPECT(note_smaa(c, x).empty()); c.aa = "ssaa2,smaa"; EXPECT(note_smaa(c, x).empty());   // SMAA in use: nothing overrides the row
    c.aa = "off"; EXPECT(note_smaa(c, x).find("Not used: the game's own FXAA") == 0); c.aa = "none"; EXPECT(note_smaa(c, x).find("Not used: anti-aliasing is off") == 0);
    Ctx o = x; o.enhanced = false; EXPECT(note_smaa(Cfg{}, o).find("Not used: the game's own FXAA") == 0);                                  // the Original look's default is FXAA
    EXPECT(note_smaa(with_aa(Cfg{}, "dlaa"), x).find("Not used: an upscaler") == 0); }

  // ---- graphics presets (runtime/hle/presets.h): derived from the stored rows, Custom after any single change
  {
    using namespace snappreset;
    std::map<std::string, std::string> kv;
    auto get = [&](const std::string& k) { auto it = kv.find(k); return it == kv.end() ? std::string() : it->second; };
    auto put = [&](const std::string& k, const std::string& v) { if (v.empty()) kv.erase(k); else kv[k] = v; };
    EXPECT(current(get) == "enhanced");                                  // a fresh install (nothing stored) is Enhanced
    for (int p = 0; p < kCount; p++) {
      for (int m = 0; m < 8; m++) {                                      // every set of installed upscalers
        Ctx c; c.dlss = m & 1; c.fsr = m & 2; c.xess = m & 4;
        kv.clear();
        apply(p, put, c);
        EXPECT(current(get) == id(p));                                   // every preset reads back as itself
        int n = 0; const Row* t = rows(&n);
        for (int i = 0; i < n; i++) {                                    // changing any single row shows Custom
          if (std::string(t[i].key) == "gfx_look") continue;
          std::map<std::string, std::string> saved = kv;
          put(t[i].key, get(t[i].key).empty() ? "7" : "");
          EXPECT(current(get) == "custom");
          kv = saved;
        }
        { std::map<std::string, std::string> saved = kv; put("gfx_aa", get("gfx_aa") == "off" ? "none" : "off"); EXPECT(current(get) == "custom"); kv = saved; }
        { std::map<std::string, std::string> saved = kv; put("gfx_upscaler", get("gfx_upscaler").empty() ? "fsr" : ""); EXPECT(current(get) == "custom"); kv = saved; }
        { std::map<std::string, std::string> saved = kv; put("gfx_look", get("gfx_look").empty() ? "original" : "");
          EXPECT(current(get) == (p == 0 ? "enhanced" : p == 2 ? "original" : "custom")); kv = saved; }   // the Look alone flips Original and Enhanced into each other
      }
    }
    // the picture each preset picks
    { Ctx c; EXPECT(picture_for(0, c).aa.empty() && picture_for(2, c).aa.empty() && picture_for(2, c).upscaler.empty()); }
    { Ctx c; c.dlss = c.fsr = c.xess = false; EXPECT(picture_for(1, c).aa == "smaa" && picture_for(1, c).upscaler.empty()); EXPECT(picture_for(3, c).aa == "ssaa2,smaa"); }
    { Ctx c; c.dlss = true; c.fsr = c.xess = false; const Cfg b = picture_for(1, c); EXPECT(b.upscaler == "dlss" && b.quality == "quality" && b.aa.empty()); EXPECT(picture_for(3, c).upscaler == "dlaa"); }
    { Ctx c; c.dlss = false; c.fsr = true; c.xess = false; EXPECT(picture_for(1, c).upscaler == "fsr" && picture_for(3, c).aa == "ssaa2,smaa"); }
    // a preset written before DLSS was installed still reads as that preset afterwards
    { Ctx none; none.dlss = none.fsr = none.xess = false; kv.clear(); apply(3, put, none); EXPECT(current(get) == "max"); Ctx all; kv.clear(); apply(3, put, all); EXPECT(current(get) == "max"); }
    // stored default spellings read as erased; numbers compare as numbers
    { kv.clear(); apply(3, put, Ctx{}); kv["leaf_translucency"] = "0.50"; kv["gfx_upscaler"] = kv["gfx_upscaler"]; EXPECT(current(get) == "max");
      kv.clear(); kv["gfx_lighting"] = "default"; kv["fps_target"] = "default"; kv["gfx_render_scale"] = "auto"; kv["gfx_smaa_quality"] = "high"; EXPECT(current(get) == "enhanced"); }
    EXPECT(from_id("max") == 3 && from_id("custom") == -1 && std::string(label(0)) == "Original" && std::string(label(-1)) == "Custom");
  }

  std::printf(g_fail ? "picture_rows_test: %d FAILED\n" : "picture_rows_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
