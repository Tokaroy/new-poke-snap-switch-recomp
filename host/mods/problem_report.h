// SPDX-License-Identifier: MIT
// Problem report (docs/player-options.md): one zip with the log files, settings files and system info a bug report needs.
// Never uploads anything: the player attaches the zip to a report themselves.
#pragma once
#include <string>

namespace snap_report {

// Writes SnapRecomp-problem-report-<date>-<time>.zip into `dir` (UTF-8; empty = SNAP_REPORT_DIR when set, else the Desktop) and returns its
// full path in *path. On failure returns false and a one-line reason in *why. `who` names the caller ("launcher" or "game") for the system file.
// What goes in: the two log files, the settings files (settings.cfg, input.json, audio.json, window.json, hle.json, config.json), the mod pack
// list, the shader cache's stats (and its pipeline key list when under 4 MB) and a system.txt (Windows, CPU, memory, GPUs with driver version,
// displays, SNAP_* variables). What stays out: game files, keys, save files, photos, the driver's pipeline cache blob. Every text file has the
// Windows user name, the computer name and user folder paths replaced before it is zipped.
bool make(const std::string& dir, const char* who, std::string* path, std::string* why);

// Scrubs one text the same way (exposed for tests): user profile paths, the user name and the computer name.
std::string scrub(const std::string& text);

}  // namespace snap_report

// For the F1 menu (runtime/ui): makes the report on the Desktop; returns the zip path, or an empty string and fills *why.
extern "C" int snap_problem_report(const char* who, char* path_out, int path_cap, char* why_out, int why_cap);
