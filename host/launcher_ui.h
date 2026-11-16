// SPDX-License-Identifier: MIT
// The themed launcher window of the game exe (v2): a screen styled like the game's own Settings menu (grey-blue panel, pointed rows, tabs with
// L and R badges, orange A and B prompts), drawn with Dear ImGui on a small D3D11 window before the game starts. Pre-launch settings
// are written straight to the same files the game reads (settings.cfg of the chosen profile, window.json, audio.json), so they apply at Play.
// Everything is drawn from shapes: no game texture, logo or font is shipped. When the game's own UI font can be decoded from the user's
// files, launcher.cpp passes its bytes in font_ttf; otherwise a system font is used.
// Design: analysis/launcher_v2/ (README.md, d_in_game_*.png).
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace snap_launcher {

struct UiProfile {
  std::string id, name;
  long long last_played = 0;   // unix seconds, 0 = never
};

struct UiInfo {
  std::string data_dir;                       // window.json and audio.json live here (SNAP_CONFIG_DIR or %APPDATA%\SnapRecomp)
  std::string profiles_root;                  // per-profile folders; empty = profiles are off
  std::string settings_noprofile;             // settings.cfg used while profiles are off
  std::vector<UiProfile> profiles;
  std::string profile_id;                     // in: preselected; out: the profile to start with
  std::function<std::string()> new_profile;   // creates a profile, returns its id ("" on failure)
  std::vector<unsigned char> font_ttf;        // the game's own UI font (decoded at run time); empty = system font
  std::string exe_dir;                        // where upscaler libraries are looked for
  std::string version;                        // builder version text
  bool game_ok = true;                        // the romfs check passed
  std::string game_msg;                       // short reason when it did not
  bool update_available = false;
  std::string update_version, update_detail;
};

enum class UiResult { Failed, Quit, Play, GameFolder, OpenSaves, ImportSave, ExportSave, Upscalers, HdTextures, Updates, ProblemReport, RestoreBackup, SafeMode };

// Shows the window and runs until the user picks something that needs the caller (or closes it). Failed = the window or D3D11 could not be
// created; the caller falls back to the task-dialog launcher. Test switches: SNAP_LAUNCHER_UI_SHOT=<file.bmp> with SNAP_LAUNCHER_UI_VIEW=
// home | settings:<tab> | pulldown:<tab>:<row> | saves renders that screen once, saves the picture and returns Quit.
UiResult show(UiInfo& info);

}  // namespace snap_launcher
