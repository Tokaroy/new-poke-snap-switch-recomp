// Portable mode: a file named portable.txt next to the exe keeps every per-user folder (settings, saves, mods, caches, logs, update state)
// under <exe dir>\portable_data instead of %APPDATA% and %LOCALAPPDATA%. Call snap_portable_init() first thing in main(), before anything reads
// those two variables. The real values stay available as SNAP_REAL_APPDATA and SNAP_REAL_LOCALAPPDATA (emulator save detection reads them).
#pragma once

// Returns true when portable mode is on (SNAP_PORTABLE=1 is then set in the environment).
// data_dir (--data-dir, host/cli_flags.h): not null and not empty = that folder holds Roaming and Local, with or without portable.txt;
// SNAP_DATA_DIR is then set to its absolute path so child processes and the builder (builder/snapbuild/userdata.py) find the same data.
bool snap_portable_init(const wchar_t* data_dir = nullptr);
