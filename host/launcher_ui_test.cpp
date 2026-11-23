// SPDX-License-Identifier: MIT
// Standalone preview of the themed launcher window (host/launcher_ui.cpp) without the game: build with host/build_launcher_ui_test.py.
// Usage: launcher_ui_test.exe [data_dir]   (settings are written under data_dir; default %TEMP%\snap_launcher_ui_test)
// Pictures: SNAP_LAUNCHER_UI_SHOT=<file.bmp> SNAP_LAUNCHER_UI_VIEW=home|settings:<tab>|pulldown:<tab>:<row>|saves
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

#include "launcher_ui.h"

int main(int argc, char** argv) {
  snap_launcher::UiInfo ui;
  std::string dir = argc > 1 ? argv[1] : std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "\\snap_launcher_ui_test";
  CreateDirectoryA(dir.c_str(), nullptr);
  ui.data_dir = dir;
  ui.profiles_root = dir + "\\profiles";
  ui.settings_noprofile = dir + "\\settings.cfg";
  ui.profiles = {{"p1", "Ash", 0}, {"p2", "Misty", 0}};
  ui.profile_id = "p1";
  ui.version = "1.0.0";
  ui.update_available = std::getenv("SNAP_TEST_UPDATE") != nullptr;
  ui.update_version = "1.1.0";
  if (const char* b = std::getenv("SNAP_TEST_BACKUP_STATUS")) ui.backup_status = b;   // the Backups page line under Back up now
  ui.new_profile = [&ui]() { ui.profiles.push_back({"p3", "Profile 3", 0}); return std::string("p3"); };
  char exe[MAX_PATH];
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string e = exe;
  ui.exe_dir = e.substr(0, e.find_last_of(char(92)));
  if (const char* f = std::getenv("SNAP_TEST_FONT")) {
    std::ifstream in(f, std::ios::binary);
    ui.font_ttf.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  auto r = snap_launcher::show(ui);
  std::printf("result %d profile %s\n", (int)r, ui.profile_id.c_str());
  return 0;
}
