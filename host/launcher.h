// Launcher mode of the game exe (host/launcher.cpp).
#pragma once
#include <filesystem>
#include <string>

namespace hle { struct Json; }

namespace snap_launcher {

struct Result {
  bool active = false;    // a config.json sits next to the exe: launcher mode
  bool proceed = false;   // start the game with the fields below; false = exit (quit, or a message was shown)
  int exit_code = 0;      // the process exit code when proceed is false (2 when the converted game files are empty or damaged)
  std::string main_elf, sdk_elf, config_path, trace_file;
  bool play = false;      // in: --play (host/cli_flags.h): no launcher window, straight to the game-folder check and the start
};

// Call when the command line holds no command of its own (player flags only). Returns false when there is no config.json next to the exe (nothing happened).
bool prepare(Result& r);

// Offers the upscaler downloads (host/launcher_upscalers.cpp). first_start = only the pre-selected one (FSR). Returns the number installed.
int offer_upscalers(const std::filesystem::path& exe_dir, const hle::Json& data, bool first_start);

}  // namespace snap_launcher
