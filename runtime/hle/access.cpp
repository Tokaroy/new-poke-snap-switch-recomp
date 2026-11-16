// SPDX-License-Identifier: MIT
// Accessibility state (runtime/hle/access.h, docs/accessibility.md). Values start from the env vars (settings.cpp exports the saved keys as env at start);
// menus change them live through set().
#include "access.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>

namespace hle::access {

namespace {
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
bool is_num(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); }); }
bool truthy(const std::string& v) { const std::string s = lower(v); return !(s.empty() || s == "0" || s == "off" || s == "false" || s == "no" || s == "default"); }

void from_env(State& s) {
  auto env = [](const char* n) { const char* e = std::getenv(n); return std::string(e ? e : ""); };
  if (int v = parse_cvd(env("SNAP_ACCESS_CVD")); v >= 0) s.cvd = v;
  if (const std::string e = env("SNAP_ACCESS_CVD_STRENGTH"); !e.empty()) s.cvd_strength = std::clamp(std::atoi(e.c_str()), 10, 100);
  s.ui_size = parse_size(env("SNAP_ACCESS_UI_SIZE"));
  s.contrast = truthy(env("SNAP_ACCESS_CONTRAST")) ? 1 : 0;
  if (int v = parse_flash(env("SNAP_ACCESS_FLASH")); v >= 0) s.flash = v;
  s.mono = truthy(env("SNAP_ACCESS_MONO")) ? 1 : 0;
}
}  // namespace

int parse_cvd(const std::string& v) {
  const std::string s = lower(v);
  if (s.empty()) return -1;
  if (s == "off" || s == "default" || s == "0" || s == "none") return 0;
  if (s == "protanopia" || s == "protan" || s == "protanomaly" || s == "1") return 1;
  if (s == "deuteranopia" || s == "deutan" || s == "deuteranomaly" || s == "2") return 2;
  if (s == "tritanopia" || s == "tritan" || s == "tritanomaly" || s == "3") return 3;
  return -1;
}
int parse_flash(const std::string& v) {
  const std::string s = lower(v);
  if (s.empty()) return -1;
  if (s == "off" || s == "default" || s == "0") return 0;
  if (s == "gentle" || s == "1") return 1;
  if (s == "standard" || s == "2") return 2;
  if (s == "strong" || s == "3") return 3;
  return -1;
}
int parse_size(const std::string& v) {
  if (!is_num(v)) return 100;
  const int n = std::atoi(v.c_str());
  if (n >= 175) return 200;
  if (n >= 138) return 150;
  if (n >= 113) return 125;
  return 100;
}

State& state() {
  static State s;
  static std::once_flag once;
  std::call_once(once, [] { from_env(s); });
  return s;
}

bool set(const std::string& key, const std::string& value) {
  State& s = state();
  if (key == "access_cvd") { const int v = parse_cvd(value); s.cvd = v < 0 ? 0 : v; }
  else if (key == "access_cvd_strength") { s.cvd_strength = value.empty() ? 100 : std::clamp(std::atoi(value.c_str()), 10, 100); }
  else if (key == "access_ui_size") s.ui_size = parse_size(value);
  else if (key == "access_contrast") s.contrast = truthy(value) ? 1 : 0;
  else if (key == "access_flash") { const int v = parse_flash(value); s.flash = v < 0 ? 0 : v; }
  else if (key == "access_mono") s.mono = truthy(value) ? 1 : 0;
  else return false;
  return true;
}

std::string get(const std::string& key) {
  const State& s = state();
  static const char* const kCvd[] = {"off", "protanopia", "deuteranopia", "tritanopia"};
  static const char* const kFlash[] = {"off", "gentle", "standard", "strong"};
  if (key == "access_cvd") return kCvd[std::clamp(s.cvd, 0, 3)];
  if (key == "access_cvd_strength") return std::to_string(s.cvd_strength);
  if (key == "access_ui_size") return std::to_string(s.ui_size);
  if (key == "access_contrast") return s.contrast ? "1" : "0";
  if (key == "access_flash") return kFlash[std::clamp(s.flash, 0, 3)];
  if (key == "access_mono") return s.mono ? "1" : "0";
  return "";
}

}  // namespace hle::access

extern "C" int snap_access_mono(void) { return hle::access::state().mono; }   // read by runtime/audio/audio_out.cpp on every mixed block
