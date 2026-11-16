// SPDX-License-Identifier: MIT
// Tests for the player cheats wiring (runtime/hle/cheats.cpp, docs/cheats.md). Included by hle_tests.cpp. The VM itself has its own synthetic suite
// (runtime/hle/cheat/tests); this checks what the game side adds: the per-cheat switches and their saved list, the master section, key-conditional
// cheats, a store that lands in a (fake) heap, and an address outside anything mapped failing the run instead of crashing. Hand-written cheat text only.
#pragma once
#include <windows.h>

#include "../../runtime/hle/cheats.h"

inline void test_cheats() {
  g_test = "cheats.wiring";
  namespace ch = hle::cheats;
  settings::use_path_for_test((g_tmp / "cheats_settings.cfg").string());
  settings::erase("cheats_enabled");
  settings::erase("cheats_on");
  _putenv_s("SNAP_CHEATS", "");
  CHECK(!ch::master_on() && !ch::active());   // off by default

  // 64 KiB of committed memory stands in for the guest heap
  uint8_t* heap = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  CHECK(heap != nullptr);
  if (!heap) return;
  std::memset(heap, 0, 0x10000);
  ch::test_hooks(reinterpret_cast<uint64_t>(heap), 0);
  auto u32 = [&](size_t off) { uint32_t v; std::memcpy(&v, heap + off, 4); return v; };

  // type 0 stores "04 1 0 00" = 4 bytes, heap region; type 8 = all key bits held (ZL is bit 8); 20000000 ends the block
  const std::string text =
      "{Always on}\n"
      "04100000 00000010 11111111\n"
      "[Plain store]\n"
      "04100000 00000020 22222222\n"
      "[Only while ZL is held]\n"
      "80000100\n"
      "04100000 00000030 33333333\n"
      "20000000\n"
      "[Out of range]\n"
      "04100000 0000FFF0 44444444\n"
      "04100000 FFFFFFF0 55555555\n";
  std::string err;
  CHECK(ch::test_load(text, &err) && err.empty());
  auto list = ch::entries();
  CHECK(list.size() == 4 && list[0].master && !list[1].master && list[1].name == "Plain store" && !list[1].on);

  CHECK(ch::test_run_once());                     // masters run, every ordinary cheat is off
  CHECK(u32(0x10) == 0x11111111u && u32(0x20) == 0 && u32(0x30) == 0);

  ch::set_on(1, true);
  CHECK(settings::get("cheats_on", "") == "Plain store");   // the enabled names are saved
  CHECK(ch::test_run_once() && u32(0x20) == 0x22222222u && u32(0x30) == 0);   // ZL not held: the conditional store stays out

  ch::test_hooks(reinterpret_cast<uint64_t>(heap), 1u << 8);
  ch::set_on(2, true);
  CHECK(settings::get("cheats_on", "") == "Plain store|Only while ZL is held");
  CHECK(ch::test_run_once() && u32(0x30) == 0x33333333u);                       // ZL held: it lands

  ch::set_on(3, true);                            // a write past the end of the committed range must fail the run, never crash
  CHECK(!ch::test_run_once());
  CHECK(u32(0xFFF0) == 0x44444444u);              // the first store was inside the committed 64 KiB and landed; the second one was not
  CHECK(!ch::entries()[3].on && ch::status().find("switched off") != std::string::npos);   // the failing cheat is switched off, the rest keep running
  CHECK(ch::test_run_once());
  ch::set_on(3, false);
  ch::set_on(1, false); ch::set_on(2, false);
  CHECK(settings::get("cheats_on", "") == "" && ch::test_run_once());

  // a mistake in the file names its line and loads nothing
  CHECK(!ch::test_load("[Broken]\n04100000 00000010\n", &err) && err.find("line") != std::string::npos);
  CHECK(ch::entries().empty());

  // the master switch and the problem-report line follow the setting
  CHECK(ch::test_load(text, &err));
  ch::set_on(1, true);
  _putenv_s("SNAP_CHEATS", "0");
  CHECK(!ch::master_on() && !ch::active());
  _putenv_s("SNAP_CHEATS", "1");
  CHECK(ch::master_on() && ch::active());
  _putenv_s("SNAP_CHEATS", "");
  settings::erase("cheats_on");
  VirtualFree(heap, 0, MEM_RELEASE);
}
