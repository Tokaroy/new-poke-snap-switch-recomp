// Pose-event / request tag probe (project code, ours). Debug tool, off unless SNAP_EVPROBE=1; with it unset nothing is registered
// and the game's own memcmp runs untouched.
//
// The game's film evaluator (main+0x418360; file offset 0x419360 in the ELF) runs when a course ends and scans each photo
// record's motion-event text for the substrings "EV_Mot_Mis" (request photo pose events) and "EV_Mot_Sp" (special pose events)
// with memcmp. With SNAP_EVPROBE=1 the memcmp import is replaced by a host version that does the same compare and, when the other
// side is one of those two literals, logs the event name found there:
//   evprobe present=<n> rec=<addr> tag=<EV_Mot_...>      (a request or special pose tag)
//   evprobe present=<n> rec=<addr> event=<EV_...>        (any other event name the evaluator checked)
// SNAP_EVPROBE=2 adds "evprobe raw ..." lines: every compare with an EV_Mot_Mis / EV_Mot_Sp text on either side (both texts, length, result, caller).
// One line per evaluated record (at most 6000 lines per run); rec is the address of the text. The tags are the names of
// MissionData.misd events (EV_Mot_Mis_Stage001_097_01, EV_Mot_Sp_P016_03 ...), so the log shows which request pose each photo on
// the film carries. Used to aim a ride at one request event. Pure logic: evprobe_core.h (tests/mods/evprobe_test.cpp).
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "loader.h"
#include "evprobe_core.h"

using namespace loader;

extern "C" uint64_t snap_present_count();   // host/boot.cpp

namespace {

std::mutex g_mu;
evprobe::Dedup g_dedup;
int g_lines;

// Readable bytes at s, up to 64.
size_t readable(const char* s) {
  if (!s) return 0;
  if (!IsBadReadPtr(s, 64)) return 64;
  size_t n = 0;
  while (n < 64 && !IsBadReadPtr(s + n, 1)) n++;
  return n;
}

// One line per evaluated record (the evaluator compares each text with both literals; the second compare is dropped).
void note(const char* p) {
  const std::string t = evprobe::name_at(p, readable(p));
  const int kind = evprobe::classify(t);
  if (!kind) return;
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_lines >= 6000 || !g_dedup.fresh((uintptr_t)p, t)) return;
  g_lines++;
  trace("evprobe present=%llu rec=%p %s=%s", (unsigned long long)snap_present_count(), (const void*)p, kind == 2 ? "tag" : "event", t.c_str());
}

int g_raw;       // SNAP_EVPROBE=2: also log every compare whose either side starts with EV_Mot_Mis / EV_Mot_Sp (both texts, length, result, caller)
int g_raw_lines;

bool ev_text(const void* p, size_t n) { return n >= 9 && !IsBadReadPtr(p, 10) && (memcmp(p, "EV_Mot_Mis", 10) == 0 || memcmp(p, "EV_Mot_Sp", 9) == 0); }
std::string g_raw_last;

void raw(const void* a, const void* b, size_t n, int r, uint64_t lr) {
  if (!ev_text(a, n) && !ev_text(b, n)) return;
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_raw_lines >= 20000) return;
  g_raw_lines++;
  const std::string ta = evprobe::name_at((const char*)a, readable((const char*)a)), tb = evprobe::name_at((const char*)b, readable((const char*)b));
  const std::string key = ta + "|" + tb + "|" + std::to_string(lr);
  if (key == g_raw_last) { g_raw_lines--; return; }
  g_raw_last = key;
  trace("evprobe raw present=%llu lr=%llx n=%zu r=%d a=%s b=%s", (unsigned long long)snap_present_count(), (unsigned long long)lr, n, r, ta.c_str(), tb.c_str());
}

void h_memcmp(Cpu* c) {
  const void* a = (const void*)(uintptr_t)c->x[0];
  const void* b = (const void*)(uintptr_t)c->x[1];
  const size_t n = (size_t)c->x[2];
  const int r = n ? memcmp(a, b, n) : 0;
  if ((n == 9 || n == 10) && !IsBadReadPtr(b, n) && evprobe::is_literal(b, n)) note((const char*)a);
  if (g_raw) raw(a, b, n, r, c->x[30]);
  c->x[0] = (uint64_t)(int64_t)r;
}

}  // namespace

namespace loader {
void snap_evprobe_install() {
  const char* e = getenv("SNAP_EVPROBE");
  if (!e || !*e || *e == '0') return;
  g_raw = *e == '2';
  add_override("memcmp", h_memcmp);
  fprintf(stderr, "[evprobe] SNAP_EVPROBE=1: logging the motion-event name of every photo record the film evaluator checks\n");
}
}  // namespace loader
