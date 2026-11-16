// SPDX-License-Identifier: MIT
// Discord Rich Presence application id (docs/player-options.md). Discord needs one from its developer portal; the project owner has not registered one,
// so the feature is dormant: with no id the presence thread never opens the pipe and the Discord rows are not shown in the launcher or the F1 menu.
// A developer can still switch it on with SNAP_DISCORD_APP_ID or the discord_app_id setting; the rows then appear.
#pragma once
#include <cstdlib>
#include <string>

namespace snapdiscord {

inline constexpr const char* kDefaultAppId = "";   // fill in only if the owner registers an application

// env > setting > built-in default; empty = no id (dormant). `setting` is the stored discord_app_id value ("" when unset).
inline std::string app_id(const std::string& setting) {
  const char* e = std::getenv("SNAP_DISCORD_APP_ID");
  if (e && *e) return e;
  if (!setting.empty()) return setting;
  return kDefaultAppId;
}

}  // namespace snapdiscord
