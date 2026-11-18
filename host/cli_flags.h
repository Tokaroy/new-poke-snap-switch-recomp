// SPDX-License-Identifier: GPL-3.0-or-later
// Player command-line flags of the game exe (docs/command-line.md). Header only and free of Windows and runtime includes, so
// tests/hle/cli_flags_test.cpp checks it on its own. host/boot.cpp main() calls parse() first, applies the result, and hands `rest`
// to the older handlers (save import/export, benchmark, shader preparation, the developer boot commands).
//
//   --play                        skip the launcher window and start the game; the process ends when the game closes
//   --safe-mode                   docs/safe-mode.md
//   --fullscreen | --borderless   borderless full-screen window on the chosen monitor (the game has no exclusive mode)
//   --windowed                    a normal window
//   --monitor <n>  --width <px>  --height <px>
//   --profile <name or id>        the profile to play (also the profile of --import-save / --export-save)
//   --set <key>=<value>           one settings key for this run only (repeatable); a name that starts with SNAP_ sets that variable
//   --data-dir <folder>           one folder for saves, settings, mods, caches and logs
//   --help | -h | /?   --version
#pragma once
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace snap_cli {

struct Parsed {
  bool help = false, version = false, play = false, safe = false;
  std::string data_dir, profile;
  std::vector<std::pair<std::string, std::string>> env;    // variables to set for this run (window flags, --set SNAP_X=v)
  std::vector<std::pair<std::string, std::string>> sets;   // --set key=value, in order
  std::vector<std::string> rest;                           // everything else, in order, for the older handlers
  std::string error;                                       // not empty: print it and exit 2
};

inline bool is_number(const std::string& s) {
  if (s.empty() || s.size() > 6) return false;
  for (char c : s) if (c < '0' || c > '9') return false;
  return true;
}

inline Parsed parse(const std::vector<std::string>& args) {
  Parsed p;
  auto value = [&](size_t& i, std::string* out) {
    if (i + 1 >= args.size()) { p.error = args[i] + " needs a value"; return false; }
    *out = args[++i];
    return true;
  };
  for (size_t i = 0; i < args.size() && p.error.empty(); i++) {
    const std::string& a = args[i];
    std::string v;
    if (a == "--help" || a == "-h" || a == "/?") p.help = true;
    else if (a == "--version") p.version = true;
    else if (a == "--play") p.play = true;
    else if (a == "--safe-mode") p.safe = true;
    else if (a == "--fullscreen" || a == "--borderless") p.env.push_back({"SNAP_WINDOW_MODE", "borderless"});
    else if (a == "--windowed") p.env.push_back({"SNAP_WINDOW_MODE", "windowed"});
    else if (a == "--monitor" || a == "--width" || a == "--height") {
      if (!value(i, &v)) break;
      if (!is_number(v)) { p.error = a + " needs a whole number, got \"" + v + "\""; break; }
      p.env.push_back({a == "--monitor" ? "SNAP_WINDOW_MONITOR" : a == "--width" ? "SNAP_WINDOW_W" : "SNAP_WINDOW_H", v});
    } else if (a == "--profile") {
      if (!value(i, &p.profile)) break;
    } else if (a == "--data-dir") {
      if (!value(i, &p.data_dir)) break;
      if (p.data_dir.empty()) p.error = "--data-dir needs a folder";
    } else if (a == "--set") {
      if (!value(i, &v)) break;
      const size_t eq = v.find('=');
      if (eq == std::string::npos || eq == 0) { p.error = "--set needs key=value, got \"" + v + "\""; break; }
      const std::string k = v.substr(0, eq), val = v.substr(eq + 1);
      if (k.compare(0, 5, "SNAP_") == 0) p.env.push_back({k, val});
      else p.sets.push_back({k, val});
    } else p.rest.push_back(a);
  }
  return p;
}

// The first "--" option in `rest` that no handler knows, or "". The lists are the options host/boot.cpp, host/benchmark.cpp and
// host/shaderprep.cpp read; an option added there must be added here.
inline std::string first_unknown(const std::vector<std::string>& rest) {
  static const char* const kTakesValue[] = {"--import-save", "--export-save", "--benchmark-view", "--courses", "--main", "--sdk", "--probe",
                                            "--iters", "--trace", "--timeout", "--stub"};
  static const char* const kPlain[] = {"--benchmark", "--benchmark-capped", "--benchmark-uncapped", "--prepare-shaders", "--force", "--visible",
                                       "--list", "--stop", "--libc", "--stop-after-init"};
  for (size_t i = 0; i < rest.size(); i++) {
    const std::string& a = rest[i];
    if (a.compare(0, 2, "--") != 0) continue;
    bool known = false;
    for (const char* k : kTakesValue) if (a == k) { known = true; i++; break; }
    if (!known) for (const char* k : kPlain) if (a == k) { known = true; break; }
    if (!known) return a;
  }
  return "";
}

// "builder_version": "<x>" out of config.json text, or "" (a development build has no config.json).
inline std::string version_from_config(const std::string& text) {
  const char* key = "\"builder_version\"";
  size_t k = text.find(key);
  if (k == std::string::npos) return "";
  size_t a = text.find('"', text.find(':', k + 17));
  if (a == std::string::npos) return "";
  size_t b = text.find('"', a + 1);
  if (b == std::string::npos) return "";
  return text.substr(a + 1, b - a - 1);
}

inline const char* help_text() {
  return "Snap Recomp\n"
         "\n"
         "  SnapRecomp.exe                    open the launcher\n"
         "  SnapRecomp.exe --play [options]   start the game without the launcher window\n"
         "\n"
         "Options:\n"
         "  --play                       skip the launcher window; the process ends when the game closes\n"
         "  --safe-mode                  mods, HD packs, enhanced effects and the shader cache off for this start\n"
         "  --fullscreen, --borderless   borderless full-screen window\n"
         "  --windowed                   normal window\n"
         "  --monitor <n>                monitor number, 0 is the first\n"
         "  --width <px> --height <px>   window size\n"
         "  --profile <name>             profile to play or to import to / export from\n"
         "  --set <key>=<value>          one setting for this start only, repeatable (keys: docs/command-line.md);\n"
         "                               a name that starts with SNAP_ sets that environment variable\n"
         "  --data-dir <folder>          keep saves, settings, mods, caches and logs in this folder\n"
         "\n"
         "Other commands (no game window unless noted):\n"
         "  --import-save <folder or zip>, --export-save <folder>\n"
         "  --benchmark [--benchmark-capped | --benchmark-uncapped] [--benchmark-view <file>]   plays the benchmark ride\n"
         "  --prepare-shaders [--force] [--visible] [--list] [--stop] [--courses <list>]\n"
         "  --help, --version\n";
}

}  // namespace snap_cli
