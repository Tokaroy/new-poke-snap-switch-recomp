// SPDX-License-Identifier: MIT
// Free camera input: hotkeys, the controller chord, movement accumulation, the idle pad while it is on, and the route-script tokens.
// The camera itself lives in host/mods/freecam.cpp. See docs/freecam.md.
#include "freecam.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace nvn { extern int g_presentCount; }   // runtime/nvn/nvn_tex_vk.cpp (the HLE tests stub it)

namespace hle::freecam {
namespace {
std::mutex g_mu;
std::atomic<bool> g_on{false}, g_enter{false}, g_leave{false};
std::atomic<int> g_last_rail{-100000};
void (*g_release)() = nullptr;
int g_enabled_cfg = -1;   // -1 = read SNAP_FREECAM once; 0 / 1 = set by the F1 option

// accumulators (hotkey and held-input state, guarded by g_mu)
Input g_in;                   // level fields are rewritten every poll, event fields accumulate until consume()
// script state: the fields the tokens of the active route lines add up to, built between script_begin and script_end
struct ScriptState {
  float move[3] = {0, 0, 0}, rate[2] = {0, 0};
  bool pose_set = false; float pose[5] = {0, 0, 0, 0, 0};
  bool fov_set = false; float fov = 0;
  int freeze = -1, speed = -1;
  int on_req = -1;          // 1 on, 0 off
  bool toggle = false, shot = false;
} g_sc, g_sc_prev;

int key_id(const char* n) { return hle::input::key_from_name(n); }

struct Edge {
  bool prev = false;
  bool rise(bool now) { bool r = now && !prev; prev = now; return r; }
};

float dead(float v, float d) {
  const float a = std::fabs(v);
  if (a <= d) return 0;
  return std::copysign((a - d) / (1.0f - d), v);
}

bool parse_floats(const char* s, float* out, int n) {
  int k = 0;
  while (*s && k < n) {
    char* e = nullptr;
    out[k++] = (float)std::strtod(s, &e);
    if (e == s) return false;
    s = e;
    if (*s == ',') ++s;
  }
  return k >= 1;
}
}  // namespace

bool enabled() {
  if (g_enabled_cfg < 0) {
    const char* e = std::getenv("SNAP_FREECAM");
    g_enabled_cfg = !(e && e[0] == '0') ? 1 : 0;
  }
  return g_enabled_cfg != 0;
}
void set_enabled(bool on) {
  g_enabled_cfg = on ? 1 : 0;
  if (!on && g_on.load()) set_active(false, "disabled in the options");
}
bool active() { return g_on.load(std::memory_order_relaxed); }
void note_rail(int present) { g_last_rail.store(present, std::memory_order_relaxed); }
bool ride_active() { return nvn::g_presentCount - g_last_rail.load(std::memory_order_relaxed) <= 30; }
void set_release_hook(void (*fn)()) { g_release = fn; }
bool wants_mouse() { return g_on.load(std::memory_order_relaxed); }

void set_active(bool on, const char* why) {
  if (on == g_on.load()) return;
  if (on && !enabled()) return;
  if (on && !ride_active()) {
    std::fprintf(stderr, "[freecam] not switched on (%s): no ride is running\n", why ? why : "");
    return;
  }
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_in = Input{};
  }
  g_on.store(on);
  (on ? g_enter : g_leave).store(true);
  if (!on && g_release) g_release();
  std::fprintf(stderr, "[freecam] %s (%s) at present %d\n", on ? "on" : "off", why ? why : "", nvn::g_presentCount);
}
bool take_enter() { return g_enter.exchange(false); }
bool take_leave() { return g_leave.exchange(false); }

void consume(Input* out) {
  std::lock_guard<std::mutex> lk(g_mu);
  *out = g_in;
  g_in.look_px[0] = g_in.look_px[1] = 0;
  g_in.speed_steps = 0;
  g_in.roll_lock_toggles = 0;
  g_in.freeze_toggles = 0;
  g_in.shots = 0;
  g_in.reset_view = false;
}

void poll(const hle::input::RawInput& raw, hle::input::PadSample& pad, hle::input::TouchSample& touch, bool menu_open, uint64_t now_ns) {
  (void)now_ns;
  using namespace hle::input;
  static const int kF6 = key_id("F6"), kF7 = key_id("F7"), kF9 = key_id("F9"), kHome = key_id("Home"), kG = key_id("G");
  static const int kW = key_id("W"), kA = key_id("A"), kS = key_id("S"), kD = key_id("D"), kSpace = key_id("Space"), kC = key_id("C");
  static const int kLCtrl = key_id("LCtrl"), kLShift = key_id("LShift"), kLAlt = key_id("LAlt"), kQ = key_id("Q"), kE = key_id("E");
  static const int kZ = key_id("Z"), kX = key_id("X"), kUp = key_id("Up"), kDown = key_id("Down"), kLeft = key_id("Left"), kRight = key_id("Right");
  static Edge e_f6, e_f7, e_f9, e_home, e_g, e_chord, e_x, e_y, e_b, e_lb, e_rb, e_wup, e_wdn, e_s_tog, e_s_shot;
  const bool listen = raw.focused && !menu_open && enabled();
  auto k = [&](int id) { return listen && id > 0 && raw.keys.test((size_t)id); };

  if (g_on.load() && !ride_active()) set_active(false, "the ride ended");   // the camera hook is not called any more after the ride
  if (e_f6.rise(k(kF6))) set_active(!g_on.load(), "F6");
  const bool chord = listen && raw.pad_connected && raw.pad.test(kPadLStick) && raw.pad.test(kPadRStick);
  if (e_chord.rise(chord)) set_active(!g_on.load(), "L3+R3");
  // a script's own on/off
  if (g_sc.on_req == 1 && !g_on.load() && ride_active()) set_active(true, "script");
  if (g_sc.on_req == 0) set_active(false, "script");
  if (e_s_tog.rise(g_sc.toggle)) set_active(!g_on.load(), "script");

  if (!g_on.load()) {   // off: the game reads the pad as mapped
    e_f7.prev = e_f9.prev = e_home.prev = e_g.prev = e_x.prev = e_y.prev = e_b.prev = e_lb.prev = e_rb.prev = e_wup.prev = e_wdn.prev = e_s_shot.prev = false;
    return;
  }

  std::lock_guard<std::mutex> lk(g_mu);
  Input& in = g_in;
  float mv[3] = {0, 0, 0};
  float look[2] = {0, 0};
  bool fast = false, slow = false;
  float zoom = 0, roll = 0;
  if (listen) {
    mv[0] = (k(kD) ? 1.f : 0.f) - (k(kA) ? 1.f : 0.f);
    mv[2] = (k(kW) ? 1.f : 0.f) - (k(kS) ? 1.f : 0.f);
    mv[1] = (k(kSpace) ? 1.f : 0.f) - ((k(kC) || k(kLCtrl)) ? 1.f : 0.f);
    fast = k(kLShift); slow = k(kLAlt);
    roll = (k(kE) ? 1.f : 0.f) - (k(kQ) ? 1.f : 0.f);
    zoom = (k(kZ) ? 1.f : 0.f) - (k(kX) ? 1.f : 0.f);
    look[0] = (k(kRight) ? 1.f : 0.f) - (k(kLeft) ? 1.f : 0.f);
    look[1] = (k(kUp) ? 1.f : 0.f) - (k(kDown) ? 1.f : 0.f);
    in.look_px[0] += raw.mouse_dx;
    in.look_px[1] += raw.mouse_dy;
    if (e_wup.rise((raw.mouse_buttons >> kMouseWheelUp) & 1)) ++in.speed_steps;
    if (e_wdn.rise((raw.mouse_buttons >> kMouseWheelDown) & 1)) --in.speed_steps;
    if (e_f7.rise(k(kF7))) ++in.freeze_toggles;
    if (e_f9.rise(k(kF9))) ++in.shots;
    if (e_home.rise(k(kHome))) in.reset_view = true;
    if (e_g.rise(k(kG))) ++in.roll_lock_toggles;
    if (raw.pad_connected) {
      mv[0] += dead(raw.lx, 0.15f);
      mv[2] += dead(raw.ly, 0.15f);
      if (raw.pad.test(kPadDUp)) mv[1] += 1.f;
      if (raw.pad.test(kPadDDown)) mv[1] -= 1.f;
      look[0] += dead(raw.rx, 0.15f);
      look[1] += dead(raw.ry, 0.15f);
      if (raw.lt > 0.3f) zoom += 1.f;
      if (raw.rt > 0.3f) zoom -= 1.f;
      if (!chord) { fast |= raw.pad.test(kPadLStick); slow |= raw.pad.test(kPadRStick); }
      if (e_lb.rise(raw.pad.test(kPadLShoulder))) --in.speed_steps;
      if (e_rb.rise(raw.pad.test(kPadRShoulder))) ++in.speed_steps;
      if (e_x.rise(raw.pad.test(kPadWest))) ++in.freeze_toggles;
      if (e_y.rise(raw.pad.test(kPadNorth))) ++in.shots;
      if (e_b.rise(raw.pad.test(kPadEast))) in.reset_view = true;
    }
  }
  // route script
  for (int i = 0; i < 3; ++i) mv[i] += g_sc.move[i];
  in.look_rate_deg[0] = g_sc.rate[0]; in.look_rate_deg[1] = g_sc.rate[1];
  if (e_s_shot.rise(g_sc.shot)) ++in.shots;
  in.pose_set = g_sc.pose_set;
  std::memcpy(in.pose, g_sc.pose, sizeof in.pose);
  in.fov_set = g_sc.fov_set; in.fov_deg = g_sc.fov;
  in.freeze_set = g_sc.freeze;
  in.speed_set = g_sc.speed;

  for (int i = 0; i < 3; ++i) in.move[i] = std::clamp(mv[i], -1.f, 1.f);
  in.look_stick[0] = std::clamp(look[0], -1.f, 1.f);
  in.look_stick[1] = std::clamp(look[1], -1.f, 1.f);
  in.fast = fast; in.slow = slow;
  in.zoom = std::clamp(zoom, -1.f, 1.f);
  in.roll = std::clamp(roll, -1.f, 1.f);

  pad = PadSample{};      // the game sees a connected, idle pad: the shutter and every other button are dead while the camera is free
  pad.connected = true;
  touch = TouchSample{};
}

// ---- route script tokens
bool script_valid_token(const std::string& t) {
  static const char* const kNames[] = {"fc=", "fcmove=", "fclook=", "fcpose=", "fcfov=", "fcfreeze=", "fcspeed="};
  if (t == "fcshot") return true;
  for (const char* n : kNames) if (t.rfind(n, 0) == 0) return true;
  return false;
}
void script_begin() { g_sc_prev = g_sc; g_sc = ScriptState{}; }
void script_token(const std::string& t) {
  const size_t eq = t.find('=');
  if (eq == std::string::npos) { if (t == "fcshot") g_sc.shot = true; return; }
  const std::string k = t.substr(0, eq), v = t.substr(eq + 1);
  float f[5] = {0, 0, 0, 0, 0};
  if (k == "fc") {
    if (v == "on") g_sc.on_req = 1; else if (v == "off") g_sc.on_req = 0; else if (v == "toggle") g_sc.toggle = true;
  } else if (k == "fcmove") {
    if (parse_floats(v.c_str(), f, 3)) for (int i = 0; i < 3; ++i) g_sc.move[i] += f[i];
  } else if (k == "fclook") {
    if (parse_floats(v.c_str(), f, 2)) { g_sc.rate[0] += f[0]; g_sc.rate[1] += f[1]; }
  } else if (k == "fcpose") {
    if (parse_floats(v.c_str(), f, 5)) { g_sc.pose_set = true; std::memcpy(g_sc.pose, f, sizeof f); }
  } else if (k == "fcfov") {
    if (parse_floats(v.c_str(), f, 1)) { g_sc.fov_set = true; g_sc.fov = f[0]; }
  } else if (k == "fcfreeze") {
    g_sc.freeze = (v == "on" || v == "1") ? 1 : 0;
  } else if (k == "fcspeed") {
    g_sc.speed = std::atoi(v.c_str());
  }
}
void script_end() {}
}  // namespace hle::freecam
