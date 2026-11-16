// SPDX-License-Identifier: MIT
// The guest heap's base address for the player cheats (runtime/hle/cheats.cpp). A cheat reads or writes "heap + offset"; the base is where the game's
// SetMemoryHeapSize put the heap.
#include <cstdint>

#include "os_internal.h"

extern "C" uint64_t snap_os_heap_base(void) {
  osk::MemBook m;
  osk::state_mem_save(m);
  return m.heap_base;
}
