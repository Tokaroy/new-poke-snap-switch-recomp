// Portable mode: a file named portable.txt next to the exe keeps every per-user folder (settings, saves, mods, caches, logs, update state)
// under <exe dir>\portable_data instead of %APPDATA% and %LOCALAPPDATA%. Call snap_portable_init() first thing in main(), before anything reads
// those two variables. The real values stay available as SNAP_REAL_APPDATA and SNAP_REAL_LOCALAPPDATA (emulator save detection reads them).
#pragma once
#include <string>

// Returns true when portable mode is on (SNAP_PORTABLE=1 is then set in the environment).
// data_dir (--data-dir, host/cli_flags.h): not null and not empty = that folder holds Roaming and Local, with or without portable.txt;
// SNAP_DATA_DIR is then set to its absolute path so child processes and the builder (builder/snapbuild/userdata.py) find the same data.
bool snap_portable_init(const wchar_t* data_dir = nullptr);

// One-time copy for a player who used %APPDATA% before portable became the default (portable.txt run, never a --data-dir run): when the
// portable data folder has no SnapRecomp folder yet and the real %APPDATA%\SnapRecomp has a save or settings, everything there except
// the regenerable shader cache and the updater's backups is COPIED (never moved; the source is only read) into the portable folder.
// The copy goes to SnapRecomp.importing and is renamed when complete, so a stopped copy is redone on the next start.
// Returns 0 = nothing to do, 1 = copied (from/to filled; mods_skipped = the mods folder did not fit on the drive and was left out),
// -1 = the copy failed (why filled; the portable folder is left without a SnapRecomp folder, so the next start tries again).
int snap_portable_adopt(std::wstring* from, std::wstring* to, bool* mods_skipped, std::string* why);
