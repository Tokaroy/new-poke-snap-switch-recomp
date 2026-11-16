// SPDX-License-Identifier: MIT
// "Cheats" tab of the F1 overlay (docs/cheats.md): the master switch and one checkbox per cheat in cheats/<build id>.txt (runtime/hle/cheats.h).
// Everything is off until the player turns the master switch on; the file is the player's own, the project ships none.
#include <cstdio>
#include <string>
#include <vector>

#include "hle/cheats.h"
#include "ui_internal.h"
#include "ui_i18n.h"
#include "ui_theme.h"

namespace snap::ui {

void tab_cheats() {
  const Theme& t = theme();
  ImGui::TextWrapped("%s", tr("Cheats are small programs in the Atmosphere cheat format that change the game's memory while it runs. Put your own cheat file in "
                     "the cheats folder next to the game (the line below says exactly where). Nothing runs until you turn the switch on, and a cheat "
                     "that is switched off does nothing. While a cheat is on, photo scores and the research level are no longer the game's own."));
  ImGui::Spacing();
  bool master = hle::cheats::master_on();
  if (ImGui::Checkbox(tr("Allow cheats"), &master)) hle::cheats::set_master(master);
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  if (!master) {
    ImGui::TextColored(t.text_dim, "%s", tr("Off. The cheat file is not even read."));
    return;
  }
  const std::vector<hle::cheats::Entry> list = hle::cheats::entries();
  const std::string status = hle::cheats::status();
  ImGui::TextColored(t.text_dim, "%s", tr(status));
  ImGui::TextColored(t.text_dim, tr("File: %s"), hle::cheats::file_path().c_str());
  if (ImGui::Button(tr("Read the file again"))) hle::cheats::reload();
  ImGui::Spacing();
  size_t shown = 0;
  for (size_t i = 0; i < list.size(); ++i) {
    const hle::cheats::Entry& e = list[i];
    ImGui::PushID((int)i);
    if (e.master) {
      ImGui::BeginDisabled();
      bool always = true;
      char lbl[256];
      std::snprintf(lbl, sizeof lbl, tr("%s  (always on)"), e.name.c_str());
      ImGui::Checkbox(lbl, &always);
      ImGui::EndDisabled();
    } else {
      bool on = e.on;
      if (ImGui::Checkbox(e.name.c_str(), &on)) hle::cheats::set_on(i, on);
    }
    ImGui::PopID();
    ++shown;
  }
  if (!shown) ImGui::TextColored(t.text_dim, "%s", tr("No cheats to list."));
}

}  // namespace snap::ui
