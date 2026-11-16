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
bool g_kbm_last = false;
std::string g_labels[256];   // key cap texts, id = index + 1 (packed row 2)
int g_nlabels = 0;
Style g_style = kAuto;
std::atomic<int> g_packed[input::kButtonCount];
std::atomic<int> g_active{kSwitch};
bool g_init = false;
std::atomic<bool> g_ready{false};   // the first read of the snapshot has run ensure_init_locked

void recompute_locked() {
  const Style act = resolve_style(g_style, g_family, g_connected, g_kbm_last);
  g_active.store(act);
  for (int b = 0; b < input::kButtonCount; b++) {   // only 0..15 (A .. Down) are ever asked for; the stick directions use another numbering in the mapping
    if (b >= 16) { g_packed[b].store(-1); continue; }
    int v = -1;
    if (act == kXbox || act == kPlayStation) {
      const int g = glyph_of_action(g_map, b);
      if (g < kGlyphCount) v = ((act == kPlayStation ? 1 : 0) << 8) | g;
    } else if (act == kKeyboard) {
      const std::string l = label_of_action(g_map, b);
      if (!l.empty()) {
        int id = 0;
        for (int i = 0; i < g_nlabels; i++) if (g_labels[i] == l) id = i + 1;
        if (!id && g_nlabels < 255) { g_labels[g_nlabels] = l; id = ++g_nlabels; }
        if (id) v = (2 << 8) | id;
      }
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
  if (l == "keyboard" || l == "kbm" || l == "key") return kKeyboard;
  if (l == "xbox") return kXbox;
  if (l == "playstation" || l == "ps" || l == "ps5" || l == "ps4") return kPlayStation;
  return kAuto;
}

const char* style_name(Style s) {
  switch (s) {
    case kSwitch: return "switch";
    case kXbox: return "xbox";
    case kPlayStation: return "playstation";
    case kKeyboard: return "keyboard";
    default: return "auto";
  }
}

Style resolve_style(Style chosen, Family detected, bool pad_connected, bool kbm_last) {
  if (chosen != kAuto) return chosen;
  if (!pad_connected || kbm_last) return kKeyboard;
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
  if (input::swap_ab_effective(m) && (action == input::kA || action == input::kB)) action = action == input::kA ? input::kB : input::kA;   // the prompt for A shows the button that now presses A
  for (const input::Source& s : input::action_sources(m, action))
    if (s.kind == input::Source::Pad) {
      const int g = glyph_of_pad_button(s.id);
      if (g < kGlyphCount) return g;
    }
  return kGlyphCount;
}

namespace {
// Key cap text: short names, arrows as arrow characters (UTF-8).
std::string key_label(int id) {
  const std::string n = input::key_name(id);
  if (n == "Up") return "\xE2\x86\x91";
  if (n == "Down") return "\xE2\x86\x93";
  if (n == "Left") return "\xE2\x86\x90";
  if (n == "Right") return "\xE2\x86\x92";
  if (n == "LShift" || n == "RShift") return "Shift";
  if (n == "LCtrl" || n == "RCtrl") return "Ctrl";
  if (n == "LAlt" || n == "RAlt") return "Alt";
  if (n == "Escape") return "Esc";
  if (n == "Backspace") return "Bksp";
  if (n == "PageUp") return "PgUp";
  if (n == "PageDown") return "PgDn";
  if (n.rfind("Numpad", 0) == 0) return "Num" + n.substr(6);
  return n;
}
std::string mouse_label(int b) {
  const std::string n = input::mouse_name(b);
  if (n == "left") return "LMB";
  if (n == "right") return "RMB";
  if (n == "middle") return "MMB";
  if (n == "x1") return "M4";
  if (n == "x2") return "M5";
  if (n == "wheel_up") return "Wheel\xE2\x86\x91";
  if (n == "wheel_down") return "Wheel\xE2\x86\x93";
  return n;
}
}  // namespace

std::string label_of_action(const input::Mapping& m, int action) {
  if (action < 0 || action >= input::kActionCount) return "";
  for (const input::Source& s : input::action_sources(m, action))
    if (s.kind == input::Source::KeyboardKey) return key_label(s.id);
  for (const input::Source& s : input::action_sources(m, action))
    if (s.kind == input::Source::Mouse) return mouse_label(s.id);
  return "";
}

std::string label_text(int id) {
  std::lock_guard<std::mutex> lk(g_mu);
  return id >= 1 && id <= g_nlabels ? g_labels[id - 1] : std::string();
}

void set_last_input(bool kbm) {
  std::lock_guard<std::mutex> lk(g_mu);
  ensure_init_locked();
  if (kbm == g_kbm_last) return;
  g_kbm_last = kbm;
  recompute_locked();
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

unsigned text_glyph_code(unsigned code) {
  if (code < 0xE0E0u || code > 0xE0E3u) return code;   // Switch A B X Y
  std::lock_guard<std::mutex> lk(g_mu);
  ensure_init_locked();
  if (resolve_style(g_style, g_family, g_connected, false) != kXbox) return code;   // not the last input: a glyph is rasterized once and then cached by the game
  static const int kActionOf[4] = {input::kA, input::kB, input::kX, input::kY};
  switch (glyph_of_action(g_map, kActionOf[code - 0xE0E0u])) {
    case kFaceBottom: return 0xE0E0u;   // Xbox A
    case kFaceRight: return 0xE0E1u;    // B
    case kFaceLeft: return 0xE0E2u;     // X
    case kFaceTop: return 0xE0E3u;      // Y
    default: return code;
  }
}

}  // namespace hle::icons

extern "C" int snap_btnicon_glyph(int button) { return hle::icons::packed_glyph(button); }
extern "C" const char* snap_btnicon_label(int id) {
  static thread_local std::string s;
  s = hle::icons::label_text(id);
  return s.c_str();
}
extern "C" int snap_btnicon_active() { return hle::icons::active_style() != hle::icons::kSwitch; }
