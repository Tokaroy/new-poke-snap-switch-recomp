// SPDX-License-Identifier: MIT
#include "btn_icons.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <mutex>

namespace hle::icons {
namespace {
std::mutex g_mu;
input::Mapping g_map = input::Mapping::defaults();
Family g_family = kFamNone;
bool g_connected = false;
Style g_style = kAuto;
std::atomic<int> g_packed[input::kButtonCount];
std::atomic<int> g_active{kSwitch};
bool g_init = false;
std::atomic<bool> g_ready{false};   // the first read of the snapshot has run ensure_init_locked

void recompute_locked() {
  const Style act = resolve_style(g_style, g_family, g_connected);
  g_active.store(act);
  for (int b = 0; b < input::kButtonCount; b++) {   // only 0..15 (A .. Down) are ever asked for; the stick directions use another numbering in the mapping
    if (b >= 16) { g_packed[b].store(-1); continue; }
    int v = -1;
    if (act == kXbox || act == kPlayStation) {
      const int g = glyph_of_action(g_map, b);
      if (g < kGlyphCount) v = ((act == kPlayStation ? 1 : 0) << 8) | g;
    }
    g_packed[b].store(v);
  }
}
// The environment variable SNAP_BUTTON_ICONS is the starting style: test runs with SNAP_SAVE_DIR never load settings.cfg, and settings::apply
// (which sets the stored value, the variable still winning) may come later or never.
void ensure_init_locked() {
  if (g_init) return;
  g_init = true;
  if (const char* e = std::getenv("SNAP_BUTTON_ICONS"); e && *e) g_style = parse_style(e);
  recompute_locked();
  g_ready.store(true);
}
void ensure_init() {
  if (g_ready.load()) return;
  std::lock_guard<std::mutex> lk(g_mu);
  ensure_init_locked();
}
}  // namespace

Style parse_style(const std::string& s) {
  std::string l;
  for (char c : s) l += (char)std::tolower((unsigned char)c);
  if (l == "switch" || l == "nintendo") return kSwitch;
  if (l == "xbox") return kXbox;
  if (l == "playstation" || l == "ps" || l == "ps5" || l == "ps4") return kPlayStation;
  return kAuto;
}

const char* style_name(Style s) {
  switch (s) {
    case kSwitch: return "switch";
    case kXbox: return "xbox";
    case kPlayStation: return "playstation";
    default: return "auto";
  }
}

Style resolve_style(Style chosen, Family detected, bool pad_connected) {
  if (chosen != kAuto) return chosen;
  if (!pad_connected) return kSwitch;
  switch (detected) {
    case kFamXbox: return kXbox;
    case kFamPlayStation: return kPlayStation;
    default: return kSwitch;   // a Switch pad, or one we cannot tell apart
  }
}

int glyph_of_pad_button(int b) {
  switch (b) {
    case input::kPadSouth: return kFaceBottom;
    case input::kPadEast: return kFaceRight;
    case input::kPadWest: return kFaceLeft;
    case input::kPadNorth: return kFaceTop;
    case input::kPadBack: return kView;
    case input::kPadStart: return kMenu;
    case input::kPadLStick: return kStickLPress;
    case input::kPadRStick: return kStickRPress;
    case input::kPadLShoulder: return kL1;
    case input::kPadRShoulder: return kR1;
    case input::kPadDUp: return kDpadUp;
    case input::kPadDDown: return kDpadDown;
    case input::kPadDLeft: return kDpadLeft;
    case input::kPadDRight: return kDpadRight;
    case input::kPadLTrigger: return kL2;
    case input::kPadRTrigger: return kR2;
    case input::kPadLStickLeft: return kStickLLeft;
    case input::kPadLStickRight: return kStickLRight;
    case input::kPadLStickUp: return kStickLUp;
    case input::kPadLStickDown: return kStickLDown;
    case input::kPadRStickLeft: return kStickRLeft;
    case input::kPadRStickRight: return kStickRRight;
    case input::kPadRStickUp: return kStickRUp;
    case input::kPadRStickDown: return kStickRDown;
    default: return kGlyphCount;   // Guide
  }
}

int glyph_of_action(const input::Mapping& m, int action) {
  if (action < 0 || action >= input::kActionCount) return kGlyphCount;
  for (const input::Source& s : input::action_sources(m, action))
    if (s.kind == input::Source::Pad) {
      const int g = glyph_of_pad_button(s.id);
      if (g < kGlyphCount) return g;
    }
  return kGlyphCount;
}

void set_mapping(const input::Mapping& m) {
  std::lock_guard<std::mutex> lk(g_mu);
  ensure_init_locked();
  g_map = m;
  recompute_locked();
}
void set_pad(Family f, bool connected) {
  std::lock_guard<std::mutex> lk(g_mu);
  ensure_init_locked();
  if (f == g_family && connected == g_connected) return;
  g_family = f;
  g_connected = connected;
  recompute_locked();
}
void set_style(Style s) {
  std::lock_guard<std::mutex> lk(g_mu);
  ensure_init_locked();
  if (s == g_style) return;
  g_style = s;
  recompute_locked();
}
Style style() { std::lock_guard<std::mutex> lk(g_mu); return g_style; }
Style active_style() { ensure_init(); return (Style)g_active.load(); }

int packed_glyph(int button) {
  if (button < 0 || button >= input::kButtonCount) return -1;
  ensure_init();
  return g_packed[button].load();
}

}  // namespace hle::icons

extern "C" int snap_btnicon_glyph(int button) { return hle::icons::packed_glyph(button); }
extern "C" int snap_btnicon_active() { return hle::icons::active_style() != hle::icons::kSwitch; }
