// SPDX-License-Identifier: MIT
// Save import and export: bring a save in from a Switch console dump (JKSV, Checkpoint: a folder of the game's save files, or a
// .zip of it) or an emulator's save folder, and take the current save out in the same plain layout.
//   Import writes the files as a NEW generation of the live save store (runtime/hle/save_gen.h layout, the same steps as the
//   runtime's commit: gens/<n>/, <n>.ok, CURRENT.tmp then an atomic replace, then the usual backup pruning), so the save that was
//   current stays as a backup. It refuses while a game holds the save folder's lock.
//   Export copies the current generation's files into a folder.
// The game's save files are named by "%08x%08x" (16 lowercase hex digits); the same names are used on the console and in emulators.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hle {
namespace saveport {

constexpr const char* kTitleId = "0100F4300BF2C000";   // the game's title ID (emulator save folder name)
constexpr int kKeepBackups = 5;                        // same default as Config::save_backups

struct FileSpec {
  const char* name;
  uint64_t size;       // size in a save written by the current game version
  uint64_t old_size;   // size in a save from a fresh older version (the game upgrades it itself on the next load); same when unchanged
};
const std::vector<FileSpec>& expected_files();   // the 15 files
void set_expected_files_for_test(const std::vector<FileSpec>* v);   // tests only: a table with small sizes (nullptr: the real one)

struct Report {
  std::vector<std::string> warnings;   // never blocks: older layout, extra files, several saves found
  std::string source;                  // where the save was found inside the input
  std::string dest;                    // import: the save folder written; export: the output folder
  uint64_t generation = 0;             // import: the new generation; export: the generation copied
  uint64_t previous = 0;               // import: the generation that was current before
  bool had_previous = false;
  uint64_t bytes = 0;
  int files = 0;
  bool old_version = false;
};

// Imports `input` (a folder or a .zip) into <save_root>/<uid_dir>/ (uid_dir empty: the one that holds a save, else the default).
// False with `err` set (a message for the player) when nothing was written; a failure never touches the current save.
// `refused_running` is set when the reason is a running game.
bool import_save(const std::string& input, const std::string& save_root, const std::string& uid_dir, Report* rep, std::string* err,
                 bool* refused_running = nullptr, int keep_backups = kKeepBackups);

// Copies the current generation's files into `dest`: straight into it when it is missing or empty, else into a new dated folder
// inside it. The files sit at the top of that folder, named as the game names them (what JKSV, Checkpoint, Ryujinx and
// yuzu-family emulators restore).
bool export_save(const std::string& save_root, const std::string& uid_dir, const std::string& dest, Report* rep, std::string* err);

struct Candidate {
  std::string label;     // "Ryujinx, save 0000000000000007"
  std::string path;      // folder to pass to import_save
  int64_t mtime = 0;     // newest file, unix seconds
  bool old_version = false;
};
// Known emulator save folders that hold this game's save (every file present, sizes plausible), newest first. `appdata` empty: %APPDATA%.
// Ryujinx: <appdata>\Ryujinx\bis\user\save\<16 hex>\0 (the save id is not mapped to the title: a folder counts when its files match
// this game's, listed with its date). yuzu family: <appdata>\<emu>\nand\user\save\0000000000000000\<user id>\<title id>.
std::vector<Candidate> detect_emulator_saves(const std::string& appdata = "");

// The save folder an import or export targets: SNAP_SAVE_DIR / save_root when set, else the profile (empty: last used one).
std::string target_save_root(const std::string& profile, std::string* label);
// uid folder name the game mounts (32 hex digits).
std::string game_uid_dir();

// Command line: cmd "import" or "export". Prints to stdout/stderr. Returns 0 ok, 1 failed, 2 refused (game running).
int run_cli(const std::string& cmd, const std::string& path, const std::string& profile);

std::string format_report(const Report& r, bool import);   // multi-line text for a dialog or the console

}  // namespace saveport
}  // namespace hle
