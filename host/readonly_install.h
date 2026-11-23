// SPDX-License-Identifier: GPL-3.0-or-later
// Start-up check for an exe in a folder the player cannot write to (docs/readonly-install.md). The pure rules are host/readonly_logic.h.
#pragma once
#include <string>

namespace snap_ro {

struct StartArgs {
  std::string data_dir;     // --data-dir (the caller chose a folder: nothing to ask)
  bool play = false;        // --play
  bool has_action = false;  // a launcher action (--update, --check-install, ...)
  bool no_prompt = false;   // --no-prompt
  bool has_rest = false;    // a developer or save command of the older handlers (--import-save, --benchmark, ...)
};

// Call in main() before snap_portable_init. Returns -1 to carry on; data_dir_out then holds the folder to hand to snap_portable_init
// (empty = nothing changes). Any other value is the process exit code: the copy was started (0), the player cancelled (1), or the folder
// cannot be written and no window may open (3; the reason is on stderr).
// Test switches: SNAP_RO_AUTOCHOICE=move:<folder> | data:<folder> answers the question without a window; SNAP_RO_NO_START=1 does not start the copy.
int startup(const StartArgs& a, std::wstring* data_dir_out);

}  // namespace snap_ro
