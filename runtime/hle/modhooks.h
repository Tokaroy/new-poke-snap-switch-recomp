// SPDX-License-Identifier: MIT
// Mod hooks (docs/mod-hooks.md): named before / after hooks on game functions, installed in the dispatch table (project code, ours).
// Host only. The core knows nothing about the loader: a hook point owns a "cell" (a GuestFn slot, in the game the dispatch-table entry
// of the function), install() puts a trampoline in it, and the trampoline runs the before hooks, the original function, then the after
// hooks. host/mods/modhooks.cpp owns the point list and the game side; tests/hle/hle_tests.cpp drives this core with fake cells.
//
// Cost: a point nobody enabled is never installed, so its cell keeps the lifted function and nothing runs. An installed point costs one
// atomic load plus the hooks.
// Crash guard: a hook that faults (access violation, divide by zero, a C++ exception ...) is disabled for the rest of the run; the guest
// registers are put back to what they were before that call and the game goes on.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../cpu.h"

namespace hle::modhooks {

constexpr size_t kMaxPoints = 64;   // trampolines are built for this many points (the point list must not grow past it)

enum class When { Before, After };

// c->x[0..7] hold the arguments in a Before hook (the hook may change them) and the result in x0 in an After hook. user is what add() got.
typedef void (*Fn)(Cpu* c, void* user);

// What a mod registers: mod id + point name identify the hook in the manifest ("<mod>/<point>").
struct Hook {
  std::string mod;       // mod id: letters, digits, '-', '_', '.'; unique per mod
  std::string point;     // a name from the point list, e.g. "Ui.SetState"
  When when = When::Before;
  Fn fn = nullptr;
  void* user = nullptr;
  bool default_on = false;   // on without a manifest entry (the manifest can still turn it off)
};

struct Stats {
  uint64_t calls = 0;     // times the hook ran (faults included)
  bool enabled = false;   // enabled by the manifest or the environment
  bool dead = false;      // faulted and was switched off
  std::string error;      // what faulted ("access violation reading 0x0 at ...")
};

// Registration (before install). False for an unknown point, a bad mod id, a null fn, or a second hook with the same mod and point.
bool add(const Hook& h);
// The point list: a fixed, ordered set of names, set once by the host (init_points) before anything registers. points()[i] has trampoline
// i. Names are case-sensitive and never change once published (docs/mod-hooks.md).
void init_points(const char* const* names, size_t n);
size_t point_count();
const char* point_name(size_t i);
int point_index(const std::string& name);   // -1 if unknown

// Manifest: <mods root>\hooks.json, {"hooks": {"<mod>/<point>": true|false, ...}}. Comments (//) are allowed. A hook that is not listed is
// off unless it was registered with default_on. Returns false (and says why in err) when the text is not valid; nothing changes then.
// env_list (SNAP_MOD_HOOKS, comma separated "<mod>/<point>" entries, "*" = every registered hook) turns hooks on after the manifest; a
// leading '-' turns one off.
bool configure(const std::string& manifest_json, const char* env_list, std::string* err);
// Enabled hooks of a point (after configure).
size_t enabled_count(size_t point);
bool point_wanted(size_t point);   // at least one enabled, live hook

// Put the trampoline of `point` in *cell (the cell must hold the original function; it is saved). False if the cell is empty, the point
// is already installed or nothing is enabled for it. uninstall puts the original back (tests; the game never calls it).
bool install(size_t point, GuestFn* cell);
void uninstall(size_t point);
bool installed(size_t point);

// Snapshot of every registered hook, for logs, tests and the doc generator.
struct Report {
  std::string mod, point;
  When when;
  Stats stats;
};
std::vector<Report> report();
// Forget everything (tests).
void reset();

}  // namespace hle::modhooks
