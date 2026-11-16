// SPDX-License-Identifier: MIT
// Player cheats (docs/cheats.md): the standalone Atmosphere-format VM in runtime/hle/cheat/ wired to this process. Off unless the player turns the master
// switch on (F1 > Cheats, settings key `cheats_enabled`, env SNAP_CHEATS=1). Cheats come from `cheats/<build id>.txt` next to the exe, written by the
// player (the project ships none). Each cheat has its own switch (settings key `cheats_on`, the enabled names joined with '|'); a cheat that is off does
// nothing, and with the master switch off the VM never runs, so photo scoring and every other rule are exactly the game's.
//
// The VM runs on the guest thread once per frame (from the Npad state read in hle/hid.cpp), so writes land where the game itself writes.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hle::cheats {

struct Entry {
  std::string name;
  bool master = false;   // a {braced} section: runs whenever the master switch is on
  bool on = false;       // a [bracketed] section the player has switched on
};

bool master_on();                  // SNAP_CHEATS=1 or the saved switch
void set_master(bool on);          // F1 menu; saves, and loads the file the first time
std::vector<Entry> entries();      // the cheats in the file (loads it if needed)
void set_on(size_t index, bool on);
void reload();                     // read the file again (the player edited it)
std::string file_path();           // where the file for this game build is looked for
std::string status();              // one line for the menu: file found, count, last error
bool active();                     // master switch on, a file loaded and something enabled in it: "cheats active" in the problem report
void tick();                       // guest thread, from the Npad read: runs the VM once per presented frame

// Tests (tests/hle/hle_tests.cpp): a text instead of the file, a fake heap base and key state instead of the game's.
bool test_load(const std::string& text, std::string* error);
void test_hooks(uint64_t heap_base, uint64_t keys);
bool test_run_once();              // one VM run now; false when it failed

}  // namespace hle::cheats
