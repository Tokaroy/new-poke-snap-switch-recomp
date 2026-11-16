// Portable mode: a file named portable.txt next to the exe keeps every per-user folder (settings, saves, mods, caches, logs, update state)
// under <exe dir>\portable_data instead of %APPDATA% and %LOCALAPPDATA%. Call snap_portable_init() first thing in main(), before anything reads
// those two variables. The real values stay available as SNAP_REAL_APPDATA and SNAP_REAL_LOCALAPPDATA (emulator save detection reads them).
#pragma once

// Returns true when portable mode is on (SNAP_PORTABLE=1 is then set in the environment).
bool snap_portable_init();
