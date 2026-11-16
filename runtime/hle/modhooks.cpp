// SPDX-License-Identifier: MIT
// Mod hooks core (docs/mod-hooks.md, runtime/hle/modhooks.h): registration, manifest, trampolines, crash guard.
#include "modhooks.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

#include "json_lite.h"

namespace hle::modhooks {
namespace {

struct Rec {
  Hook h;
  bool on = false;                       // set by configure(), read by install()
  std::atomic<bool> dead{false};
  std::atomic<uint64_t> calls{0};
  std::string err;                       // written once, by the thread that wins the dead flag
};

struct Chain {
  std::vector<Rec*> before, after;
};

struct Slot {
  GuestFn orig = nullptr;
  GuestFn* cell = nullptr;
  const Chain* chain = nullptr;          // immutable once installed
};

std::vector<std::string> g_names;
std::vector<std::unique_ptr<Rec>> g_recs;
Slot g_slot[kMaxPoints];

bool valid_mod_id(const std::string& s) {
  if (s.empty() || s.size() > 48) return false;
  for (char ch : s) {
    bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
    if (!ok) return false;
  }
  return true;
}

std::string key_of(const Rec& r) { return r.h.mod + "/" + r.h.point; }

// ---- the guarded call. SEH and C++ unwinding cannot share a function, so the C++ catch sits one level down.
struct Fault {
  bool hit = false;
  unsigned code = 0;
  uint64_t addr = 0, info0 = 0, info1 = 0;
  char what[160] = {0};
};

int CallCxx(Rec* r, Cpu* c, Fault* f) {
  try {
    r->h.fn(c, r->h.user);
    return 0;
  } catch (const std::exception& e) {
    f->hit = true;
    snprintf(f->what, sizeof f->what, "C++ exception: %s", e.what());
  } catch (...) {
    f->hit = true;
    snprintf(f->what, sizeof f->what, "C++ exception (not std::exception)");
  }
  return 1;
}

#ifdef _WIN32
int Capture(EXCEPTION_POINTERS* ep, Fault* f) {
  f->hit = true;
  f->code = ep->ExceptionRecord->ExceptionCode;
  f->addr = (uint64_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
  if (ep->ExceptionRecord->NumberParameters >= 2) {
    f->info0 = ep->ExceptionRecord->ExceptionInformation[0];
    f->info1 = ep->ExceptionRecord->ExceptionInformation[1];
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

int CallGuarded(Rec* r, Cpu* c, Fault* f) {
  __try {
    return CallCxx(r, c, f);
  } __except (Capture(GetExceptionInformation(), f)) {
    return 2;
  }
}
#else
int CallGuarded(Rec* r, Cpu* c, Fault* f) { return CallCxx(r, c, f); }
#endif

void Kill(Rec* r, const Fault& f) {
  bool exp = false;
  if (!r->dead.compare_exchange_strong(exp, true)) return;   // another thread already reported it
  char msg[256];
  if (f.what[0]) snprintf(msg, sizeof msg, "%s", f.what);
  else if (f.code == 0xC0000005) snprintf(msg, sizeof msg, "access violation %s %#llx at %#llx", f.info0 == 1 ? "writing" : f.info0 == 8 ? "executing" : "reading",
                                          (unsigned long long)f.info1, (unsigned long long)f.addr);
  else snprintf(msg, sizeof msg, "exception %#x at %#llx", f.code, (unsigned long long)f.addr);
  r->err = msg;
  fprintf(stderr, "[modhooks] %s faulted (%s): hook disabled for this run\n", key_of(*r).c_str(), msg);
  fflush(stderr);
}

void CallHook(Rec* r, Cpu* c) {
  if (r->dead.load(std::memory_order_relaxed)) return;
  r->calls.fetch_add(1, std::memory_order_relaxed);
  // the hook may have clobbered registers before it faulted: put back what the guest function would have seen
  uint64_t x[31], sp = c->sp, pc = c->pc, nz = c->nzcv;
  memcpy(x, c->x, sizeof x);
  Fault f;
  if (CallGuarded(r, c, &f) == 0) return;
  memcpy(c->x, x, sizeof x);
  c->sp = sp; c->pc = pc; c->nzcv = nz;
  Kill(r, f);
}

void Run(size_t i, Cpu* c) {
  const Slot& s = g_slot[i];
  const Chain* ch = s.chain;
  for (Rec* r : ch->before) CallHook(r, c);
  s.orig(c);
  if (RESUMING(c)) return;   // an exception or longjmp is passing through the function: the after hooks never see it
  for (Rec* r : ch->after) CallHook(r, c);
}

template <size_t I>
void Tramp(Cpu* c) { Run(I, c); }

template <size_t... Is>
constexpr std::array<GuestFn, sizeof...(Is)> MakeTramps(std::index_sequence<Is...>) { return {{&Tramp<Is>...}}; }
const auto kTramps = MakeTramps(std::make_index_sequence<kMaxPoints>{});

}  // namespace

void init_points(const char* const* names, size_t n) {
  g_names.clear();
  for (size_t i = 0; i < n && i < kMaxPoints; i++) g_names.push_back(names[i]);
}
size_t point_count() { return g_names.size(); }
const char* point_name(size_t i) { return i < g_names.size() ? g_names[i].c_str() : ""; }
int point_index(const std::string& name) {
  for (size_t i = 0; i < g_names.size(); i++) if (g_names[i] == name) return (int)i;
  return -1;
}

bool add(const Hook& h) {
  if (!valid_mod_id(h.mod) || !h.fn) return false;
  int p = point_index(h.point);
  if (p < 0 || g_slot[p].chain) return false;   // unknown point, or the point is already installed
  for (auto& r : g_recs) if (r->h.mod == h.mod && r->h.point == h.point) return false;
  auto r = std::make_unique<Rec>();
  r->h = h;
  r->on = h.default_on;
  g_recs.push_back(std::move(r));
  return true;
}

bool configure(const std::string& manifest_json, const char* env_list, std::string* err) {
  std::vector<std::pair<std::string, bool>> set;
  if (!manifest_json.empty()) {
    Json j;
    std::string e;
    if (!json_parse(manifest_json, j, e)) { if (err) *err = "hooks.json: " + e; return false; }
    if (j.type != Json::Obj) { if (err) *err = "hooks.json: the top level must be an object"; return false; }
    if (const Json* hk = j.get("hooks")) {
      if (hk->type != Json::Obj) { if (err) *err = "hooks.json: \"hooks\" must be an object of \"<mod>/<point>\": true|false"; return false; }
      for (auto& kv : hk->o) {
        if (kv.second.type != Json::Bool) { if (err) *err = "hooks.json: \"" + kv.first + "\" must be true or false"; return false; }
        set.emplace_back(kv.first, kv.second.b);
      }
    }
  }
  if (env_list) {
    std::string s = env_list;
    size_t i = 0;
    while (i <= s.size()) {
      size_t j = s.find(',', i);
      if (j == std::string::npos) j = s.size();
      std::string t = s.substr(i, j - i);
      while (!t.empty() && t.front() == ' ') t.erase(t.begin());
      while (!t.empty() && t.back() == ' ') t.pop_back();
      if (!t.empty()) {
        bool on = true;
        if (t[0] == '-') { on = false; t.erase(t.begin()); }
        set.emplace_back(t, on);
      }
      i = j + 1;
    }
  }
  for (auto& kv : set) {
    for (auto& r : g_recs) {
      if (kv.first == "*" || key_of(*r) == kv.first) r->on = kv.second;
    }
  }
  return true;
}

size_t enabled_count(size_t point) {
  size_t n = 0;
  for (auto& r : g_recs) if (r->on && !r->dead && point < g_names.size() && r->h.point == g_names[point]) n++;
  return n;
}
bool point_wanted(size_t point) { return enabled_count(point) > 0; }

bool install(size_t point, GuestFn* cell) {
  if (point >= g_names.size() || !cell || !*cell || g_slot[point].chain) return false;
  auto* ch = new Chain();
  for (auto& r : g_recs) {
    if (!r->on || r->dead || r->h.point != g_names[point]) continue;
    (r->h.when == When::Before ? ch->before : ch->after).push_back(r.get());
  }
  if (ch->before.empty() && ch->after.empty()) { delete ch; return false; }
  Slot& s = g_slot[point];
  s.orig = *cell;
  s.cell = cell;
  s.chain = ch;
  *cell = kTramps[point];
  return true;
}

void uninstall(size_t point) {
  if (point >= g_names.size()) return;
  Slot& s = g_slot[point];
  if (!s.chain) return;
  *s.cell = s.orig;
  // the chain is leaked on purpose: a thread may still be inside the trampoline
  s = Slot{};
}

bool installed(size_t point) { return point < kMaxPoints && g_slot[point].chain != nullptr; }

std::vector<Report> report() {
  std::vector<Report> out;
  for (auto& r : g_recs) {
    Report x;
    x.mod = r->h.mod; x.point = r->h.point; x.when = r->h.when;
    x.stats.calls = r->calls.load();
    x.stats.enabled = r->on;
    x.stats.dead = r->dead.load();
    x.stats.error = r->err;
    out.push_back(std::move(x));
  }
  return out;
}

void reset() {
  for (size_t i = 0; i < kMaxPoints; i++) if (g_slot[i].chain) uninstall(i);
  g_recs.clear();
  g_names.clear();
}

}  // namespace hle::modhooks
