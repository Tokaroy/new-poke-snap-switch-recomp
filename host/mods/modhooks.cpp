// Mod hooks, game side (project code, ours). Docs: docs/mod-hooks.md. Core (registration, manifest, trampolines, crash guard):
// runtime/hle/modhooks.{h,cpp}.
//
// This file owns the stable point list (name -> main module offset), registers the bundled sample mods, reads <mods root>\hooks.json and
// SNAP_MOD_HOOKS, and for every point that has an enabled hook swaps the function's dispatch-table cell for the trampoline. Nothing is
// installed for a point nobody enabled, so with no hook set the game runs exactly the code it ran before.
//
// Reach: the dispatch table is read by indirect calls (vtables, function pointers, std::function, PLT calls from main into sdk). A direct
// lifted call (`f_<addr>(c)`, a bl to the function's own start inside main) skips it. install logs how many direct callers each point has
// in main's text; the point list only holds functions with none, and the doc says how to add one that has some.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "loader.h"
#include "../../runtime/hle/modhooks.h"

namespace mods { std::string default_root(); }   // runtime/mods/mods_core.cpp

namespace {

namespace mh = hle::modhooks;
constexpr uint64_t kMainBase = 0x7100000000ull;

// The stable list. Names never change or move once published; new points are appended. Offsets are main module offsets (the numbers the
// other host mods and docs/mod-ideas.md use). args: what the registers hold at the entry of the function (Before) / its result (After).
struct PointDef {
  const char* name;
  uint64_t off;
};
const PointDef kPoints[] = {
    {"Camera.RideBuild", 0x295170},   // x0 = camera object (eye / target / up at +0xd0 / +0xe0 / +0xf0 after it ran); every frame of a ride
    {"Ui.Event", 0x6c1d90},           // x1 = int* event id, x2 = Player**, x3 = std::string* state; the UI scene base's std::function operator()
    {"Ui.OptionEvent", 0x728e10},     // x3 = std::string* tab / element name; the option screen's event handler
    {"Ui.SetState", 0x79a6d0},        // x0 = player, x1 = page descriptor (state name, a libc++ std::string, at +0x18); Player::SetState
};
constexpr size_t kPointCount = sizeof kPoints / sizeof kPoints[0];
static_assert(kPointCount <= mh::kMaxPoints, "more points than trampolines");

// libc++ std::string: short form byte 0 = size * 2 and text at +1; long form byte 0 bit 0 set, size at +8, data pointer at +0x10.
bool read_str(uint64_t s, char* out, size_t cap) {
  out[0] = 0;
  if (!s || IsBadReadPtr((void*)s, 24)) return false;
  uint8_t b0 = *(const uint8_t*)s;
  const char* p;
  size_t n;
  if (b0 & 1) {
    n = *(const uint64_t*)(s + 8);
    p = *(const char* const*)(s + 16);
    if (n > 4096 || !p || IsBadReadPtr(p, n ? n : 1)) return false;
  } else {
    n = b0 >> 1;
    p = (const char*)(s + 1);
  }
  if (n >= cap) n = cap - 1;
  memcpy(out, p, n);
  out[n] = 0;
  return true;
}

// ---- bundled sample mods (off unless hooks.json or SNAP_MOD_HOOKS turns them on)

// sample-ui-log / Ui.SetState: logs each page the game's UI moves to (the first 300). `Ui.SetState` takes the page descriptor in x1.
void sample_ui_log(Cpu* c, void*) {
  static std::atomic<int> n{0};
  int k = n.fetch_add(1);
  if (k >= 300) return;
  char st[96];
  if (!read_str(c->x[1] + 0x18, st, sizeof st)) strcpy(st, "?");
  fprintf(stderr, "[sample-ui-log] #%d Ui.SetState player=%#llx state=%s\n", k + 1, (unsigned long long)c->x[0], st);
}

// sample-ride-camera / Camera.RideBuild (After): logs the ride camera's eye once a second of builds (60 builds).
void sample_ride_camera(Cpu* c, void*) {
  static std::atomic<uint32_t> n{0};
  if (n.fetch_add(1) % 60) return;
  uint64_t obj = c->x[0];
  if (!obj || IsBadReadPtr((void*)(obj + 0xd0), 12)) return;
  const float* e = (const float*)(obj + 0xd0);
  fprintf(stderr, "[sample-ride-camera] eye %.2f %.2f %.2f\n", e[0], e[1], e[2]);
}

// sample-fault / Ui.Event: reads a null pointer on its first call, to show the crash guard (SNAP_MOD_HOOKS=sample-fault/Ui.Event). Never on by default.
void sample_fault(Cpu*, void*) {
  volatile int* p = nullptr;
  *p = 1;
}

// Direct callers (bl / b) of each wanted address in main's text: the calls a dispatch-table hook does not see.
std::vector<int> count_direct_callers(const std::vector<uint64_t>& targets) {
  std::vector<int> n(targets.size(), 0);
  const uint64_t lo = loader::g_main.base + loader::g_main.text_off;
  const uint32_t* w = (const uint32_t*)lo;
  const size_t words = (size_t)(loader::g_main.text_size >> 2);
  if (!words || IsBadReadPtr(w, words * 4)) return n;
  for (size_t i = 0; i < words; i++) {
    uint32_t v = w[i];
    uint32_t op = v & 0xFC000000u;
    if (op != 0x94000000u && op != 0x14000000u) continue;
    int64_t imm = (int64_t)(int32_t)((v & 0x03FFFFFFu) << 6) >> 6;
    uint64_t t = lo + ((uint64_t)i << 2) + (uint64_t)(imm * 4);
    for (size_t k = 0; k < targets.size(); k++) if (t == targets[k]) n[k]++;
  }
  return n;
}

std::string read_text(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return "";
  std::string s;
  char buf[4096];
  size_t k;
  while ((k = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, k);
  fclose(f);
  return s;
}

}  // namespace

// Called once, after load() and before the guest runs. Cheap when no hook is enabled: one failed file open and one getenv.
extern "C" void snap_modhooks_install() {
  const char* names[kPointCount];
  for (size_t i = 0; i < kPointCount; i++) names[i] = kPoints[i].name;
  mh::init_points(names, kPointCount);

  auto reg = [](const char* mod, const char* point, mh::When w, mh::Fn fn) {
    mh::Hook h;
    h.mod = mod; h.point = point; h.when = w; h.fn = fn;
    mh::add(h);
  };
  reg("sample-ui-log", "Ui.SetState", mh::When::Before, sample_ui_log);
  reg("sample-ride-camera", "Camera.RideBuild", mh::When::After, sample_ride_camera);
  reg("sample-fault", "Ui.Event", mh::When::Before, sample_fault);

  const std::string manifest_path = mods::default_root() + "\\hooks.json";
  std::string manifest = read_text(manifest_path);
  const char* env = getenv("SNAP_MOD_HOOKS");
  if (manifest.empty() && !(env && *env)) return;   // nothing enabled: no point is touched
  std::string err;
  if (!mh::configure(manifest, env, &err)) {
    fprintf(stderr, "[modhooks] %s (%s): no hook is on\n", err.c_str(), manifest_path.c_str());
    return;
  }
  std::vector<size_t> wanted;
  std::vector<uint64_t> addrs;
  for (size_t i = 0; i < kPointCount; i++) {
    if (!mh::point_wanted(i)) continue;
    wanted.push_back(i);
    addrs.push_back(kMainBase + kPoints[i].off);
  }
  if (wanted.empty()) return;
  std::vector<int> direct = count_direct_callers(addrs);
  int installed = 0;
  for (size_t k = 0; k < wanted.size(); k++) {
    size_t i = wanted[k];
    GuestFn* cell = loader::dtab_cell(addrs[k]);
    if (!cell) { fprintf(stderr, "[modhooks] %s: main+0x%llx is not a lifted function start: skipped\n", kPoints[i].name, (unsigned long long)kPoints[i].off); continue; }
    if (!mh::install(i, cell)) continue;
    installed++;
    fprintf(stderr, "[modhooks] %s (main+0x%llx): %zu hook(s) on%s\n", kPoints[i].name, (unsigned long long)kPoints[i].off, mh::enabled_count(i),
            direct[k] ? "" : "; no direct callers, every call is seen");
    if (direct[k]) fprintf(stderr, "[modhooks] %s: %d direct caller(s) in main skip the dispatch table and are not hooked\n", kPoints[i].name, direct[k]);
  }
  if (installed) loader::refresh_import_fns();
}
