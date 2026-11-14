// Launcher mode of the game exe (host/launcher.cpp).
#pragma once
#include <string>

namespace hle { struct Json; }

namespace snap_launcher {

struct Result {
  bool active = false;    // a config.json sits next to the exe: launcher mode
  bool proceed = false;   // start the game with the fields below; false = exit (quit, or a message was shown)
  std::string main_elf, sdk_elf, config_path, trace_file;
};

// Call with no command-line arguments. Returns false when there is no config.json next to the exe (nothing happened).
bool prepare(Result& r);

}  // namespace snap_launcher
