// SPDX-License-Identifier: GPL-3.0-or-later
// Crash resume (docs/crash-resume.md). Header only, so tests/hle/hle_tests.cpp checks it without the host.
//
// While the game runs, host/mods/crashlog.cpp keeps <crash dir>\session.marker: who owns it and the last screen the game opened. A clean exit removes
// it. A marker whose owner is gone means the last session ended without closing (a crash, a kill, a power cut); the launcher then says so and offers
// to start again, to open the problem report, to put back the last save backup when the save looks damaged, and to skip the title screen.
// Nothing here writes to the game's save, and nothing changes what the game does: resuming only presses the same buttons a player would.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>


namespace snap_resume {

struct Session {
  uint64_t pid = 0, start = 0;   // the owner (start = process creation time), so a reused pid is not mistaken for it
  std::string scene;             // the last UI scene the game opened ("map_select", ...), "" before the first one
  long long when = 0;            // unix seconds of the last update
};

inline std::string format(const Session& s) {
  return "pid=" + std::to_string(s.pid) + "\r\nstart=" + std::to_string(s.start) + "\r\nscene=" + s.scene + "\r\nwhen=" + std::to_string(s.when) + "\r\n";
}

// False when there is no usable pid (an empty or torn file): the caller treats that as an unreadable marker, not as no marker.
inline bool parse(const std::string& text, Session* out) {
  Session s;
  auto field = [&](const char* key, std::string* v) {
    const size_t k = text.find(key);
    if (k == std::string::npos || (k && text[k - 1] != '\n')) return false;
    size_t e = text.find_first_of("\r\n", k);
    *v = text.substr(k + std::strlen(key), e == std::string::npos ? std::string::npos : e - k - std::strlen(key));
    return true;
  };
  std::string v;
  if (!field("pid=", &v) || v.empty() || v.find_first_not_of("0123456789") != std::string::npos) return false;
  s.pid = std::strtoull(v.c_str(), nullptr, 10);
  if (field("start=", &v)) s.start = std::strtoull(v.c_str(), nullptr, 10);
  if (field("scene=", &v)) { for (char c : v) if (c >= 32 && c < 127) s.scene += c; if (s.scene.size() > 40) s.scene.resize(40); }
  if (field("when=", &v)) s.when = std::strtoll(v.c_str(), nullptr, 10);
  *out = s;
  return true;
}

// Where "resume" goes: the lab hub menu ("main_menu") or the course map ("map_select"). The game's own save decides which courses are open
// there, so skipping the menus never skips progress.
inline const char* target_for(const std::string& scene) {
  static const char* const kMap[] = {"map_select", "photo_submit", "evaluation", "result_reward", "myphoto"};
  for (const char* m : kMap) if (scene == m) return "map_select";
  return scene.empty() || scene == "title" ? "" : "main_menu";   // never got past the title: nothing to skip to
}

inline std::string scene_label(const std::string& scene) {
  if (scene == "title") return "the title screen";
  if (scene == "main_menu") return "the lab menu";
  if (scene == "map_select") return "the course map";
  if (scene == "photo_submit") return "photo selection";
  if (scene == "evaluation" || scene == "result_reward") return "the photo evaluation";
  if (scene == "myphoto") return "the photo album";
  if (scene == "option") return "the options";
  if (scene == "pause_menu") return "the pause menu";
  return scene.empty() ? "" : "a menu";
}

// The input script of a resume start (the format of host/README.md "Scripted input"): slow A taps through the title screen and, for the course
// map, the lab menu, until the target scene opens. Real input stays active and is OR-ed in; the script presses nothing once it is there.
// A wait that runs out does not end the game: the starter sets SNAP_ROUTE_TIMEOUT=continue.
inline std::string script_for(const std::string& target) {
  if (target.empty()) return "";
  std::string t = "# Crash resume (host/crash_resume.h): from the title screen to " + target + ". Written by the launcher; safe to delete.\n";
  t += "repeat @f300 @f30000 120 8 A\n";
  t += "wait ui=main_menu timeout=240\n";
  if (target == "map_select") {
    t += "repeat @f60 @f30000 120 8 A\n";
    t += "wait ui=map_select timeout=240\n";
  }
  return t;
}

}  // namespace snap_resume
