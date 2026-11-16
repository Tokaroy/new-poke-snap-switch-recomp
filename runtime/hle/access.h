// SPDX-License-Identifier: MIT
// Accessibility options (docs/accessibility.md): one small state block the host layers read every frame. Every row has an env var and a settings key
// (hle/settings.cpp exports the saved key as the env var at start, unless the variable is already set by hand), and the F1 Accessibility tab and the
// launcher write the key and push the new value in here at once, so a change shows on the next frame. All rows default to 0 = the game as it ships.
//
//   row                  env var                      settings key            values
//   colour vision        SNAP_ACCESS_CVD              access_cvd              off | protanopia | deuteranopia | tritanopia   (0..3 also read)
//   ... strength         SNAP_ACCESS_CVD_STRENGTH     access_cvd_strength     10..100 (percent, default 100)
//   menu text size       SNAP_ACCESS_UI_SIZE          access_ui_size          100 | 125 | 150 | 200 (percent)
//   high contrast        SNAP_ACCESS_CONTRAST         access_contrast         0 | 1
//   reduce flashing      SNAP_ACCESS_FLASH            access_flash            off | gentle | standard | strong (0..3 also read)
//   mono audio           SNAP_ACCESS_MONO             access_mono             0 | 1
//   hold-to-toggle       SNAP_ACCESS_TOGGLE           input.json toggle_buttons   comma list of ZL ZR L R X Y B (input layer, hle/input_core.cpp)
#pragma once
#include <string>

namespace hle::access {

struct State {
  int cvd = 0;             // 0 off, 1 protanopia, 2 deuteranopia, 3 tritanopia
  int cvd_strength = 100;  // 10..100
  int ui_size = 100;       // 100, 125, 150, 200 (percent) for our own menus: the launcher and the F1 menu
  int contrast = 0;        // high-contrast theme for our own menus
  int flash = 0;           // 0 off, 1 gentle, 2 standard, 3 strong
  int mono = 0;
};

State& state();   // read from the env vars on first use; plain ints, read every frame by the present path (a torn read is harmless)
// Live change from a menu. Returns false for an unknown key. `value` is the stored text ("" = the row's default).
bool set(const std::string& key, const std::string& value);
std::string get(const std::string& key);   // the stored text of a row's current value ("off", "100", ...)
// Pure parsers (tests): text -> index, -1 = not recognized.
int parse_cvd(const std::string& v);
int parse_flash(const std::string& v);
int parse_size(const std::string& v);   // 100 / 125 / 150 / 200, snapped; 100 for anything else

}  // namespace hle::access
