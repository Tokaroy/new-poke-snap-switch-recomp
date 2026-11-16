// SPDX-License-Identifier: MIT
// Install check (docs/player-options.md, builder/snapbuild/installcheck.py is the Python twin): compares the player's extracted RomFS, the
// built program folder (exe, DLLs, converted game images) and the program files used for the build against hash lists, and says in plain
// words what is wrong: modified, wrong version, missing or a mixed update. Only hashes are compared and none is ever shown.
//
// Reads, next to the exe: config.json (romfs_root, main_elf, sdk_elf), launcher_data.json (the 2.0.1 romfs check files and the file-list
// hash, "install": the known-good shared DLL hashes; written by the builder from builder/versions.json and builder/install_hashes.json) and
// install_record.json (size and hash of the exe, DLLs, converted images and program files at build time).
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace snap_install {

struct Report {
  bool ok = false;                  // true only when nothing is wrong
  std::string summary;              // one line
  std::vector<std::string> lines;   // one line per part checked, "ok: ..." or "PROBLEM: ..."
};

// Runs the check for the install in exe_dir (about a second; reads the exe, the DLLs and the converted images, only stats the RomFS files).
Report run(const std::filesystem::path& exe_dir);

// The summary, then the lines, as plain text (LF line ends).
std::string text(const Report& r);

}  // namespace snap_install

// For the problem report: the check as text for the install next to the running exe. Returns the length written (0 on failure).
extern "C" int snap_install_check_text(char* out, int cap);
