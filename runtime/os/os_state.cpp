// Save states, part 1: drain the virtual cores and park every guest thread at a clean point (docs/save-states.md section 2.2).
// This file holds the drain/park machinery and the M1 census (SNAP_STATE_TEST=census@t<seconds>[,n<rounds>][,gap<seconds>]).
// Copy, capture and restore build on it (os_state_snap.cpp). Game behavior is unchanged while g_sphase is 0.
#include "os_internal.h"
#include "os_layout.h"
#include <windows.h>
#include <dbghelp.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" void snap_os_mem_stats(uint64_t *out);

using namespace oslay;

namespace osk {

std::atomic<int> g_sphase{0};   // 0 idle, 1 drain: no thread is granted a core at a clean site

namespace {
enum { SITE_NONE = 0, SITE_WAIT = 1, SITE_ACQ = 2 };
constexpr int kFrames = 40;
struct Park {
    uint64_t round = 0;
    int site = SITE_NONE;
    int n = 0;
    void *fr[kFrames];
    uint64_t t_us = 0;
};
std::unordered_map<Thread *, Park> g_park;   // under G
std::condition_variable g_coord;             // the coordinator waits on it with G
uint64_t g_round = 0;                        // under G
uint64_t g_unclean_grants = 0;

uint64_t now_us() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void note(Thread *t, int site) {
    Park &p = g_park[t];
    if (p.round == g_round) return;
    p.round = g_round;
    p.site = site;
    p.t_us = now_us();
    p.n = (int)RtlCaptureStackBackTrace(0, kFrames, p.fr, nullptr);
    g_coord.notify_all();
}

// ---- symbols (census only): dbghelp loaded at run time so the build needs no new library
struct Sym {
    HMODULE m = nullptr;
    BOOL(WINAPI *init)(HANDLE, PCSTR, BOOL) = nullptr;
    BOOL(WINAPI *from)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO) = nullptr;
    bool ok = false;
    void load() {
        if (m) return;
        m = LoadLibraryA("dbghelp.dll");
        if (!m) return;
        init = (decltype(init))GetProcAddress(m, "SymInitialize");
        from = (decltype(from))GetProcAddress(m, "SymFromAddr");
        auto opt = (DWORD(WINAPI *)(DWORD))GetProcAddress(m, "SymSetOptions");
        if (opt) opt(0x2 | 0x4 | 0x10);   // UNDNAME, DEFERRED_LOADS, LOAD_LINES
        if (init) init(GetCurrentProcess(), nullptr, TRUE);   // fails when the crash reporter initialised dbghelp already: its session works for us
        ok = init && from;
    }
    std::string name(void *a) {
        char buf[sizeof(SYMBOL_INFO) + 200];
        SYMBOL_INFO *s = (SYMBOL_INFO *)buf;
        memset(s, 0, sizeof *s);
        s->SizeOfStruct = sizeof(SYMBOL_INFO);
        s->MaxNameLen = 190;
        DWORD64 d = 0;
        char out[260];
        if (ok && from(GetCurrentProcess(), (DWORD64)(uintptr_t)a, &d, s)) {
            snprintf(out, sizeof out, "%s+%#llx", s->Name, (unsigned long long)d);
        } else {
            snprintf(out, sizeof out, "%p", a);
        }
        return out;
    }
} g_sym;

const char *tname(const Thread *t) {
    uint64_t p = t->tt ? gld<uint64_t>(t->tt + TT_NAME_POINTER) : 0;
    return p ? (const char *)(uintptr_t)p : "?";
}

// Frames that belong to the scheduler machinery or the thread start: not interesting in the census.
bool boring(const std::string &s) {
    static const char *skip[] = {"state_gate", "note", "RtlCaptureStackBackTrace", "wait_woken", "acquire_core", "std::", "_Cnd", "RtlUserThreadStart",
                                 "BaseThreadInitThunk", "_beginthreadex", "_callthreadstartex", "thread_start", "host_entry"};
    for (const char *k : skip)
        if (s.find(k) != std::string::npos) return true;
    return false;
}

struct Row {
    std::string name;
    uint64_t id;
    int prio, wkind, core, site, unclean;
    bool host_blocked, on_fiber;
    double park_ms;
    std::vector<void *> fr;
};

// One drain: returns true when every started guest thread is parked within timeout_ms.
bool drain(int timeout_ms, std::vector<Row> &rows, double &drain_ms, std::string &last) {
    const uint64_t t0 = now_us();
    std::vector<uint64_t> tts = all_thread_types();
    Lock lk(G);
    g_round++;
    g_unclean_grants = 0;
    g_sphase.store(1);
    bool ok = false;
    uint64_t last_kick = 0;
    for (;;) {
        bool all = true;
        last.clear();
        for (uint64_t tt : tts) {
            Thread *t = find_tt(tt);
            if (!t || t->is_host || t->exited || !t->started) continue;
            auto it = g_park.find(t);
            bool noted = it != g_park.end() && it->second.round == g_round;
            if (noted && t->core < 0) continue;
            all = false;
            if (last.size() < 200) last += std::string(tname(t)) + (t->core >= 0 ? "(core)" : t->block_depth ? "(hostblk)" : t->wkind ? "(wait)" : "(run)") + " ";
            if (t->core >= 0) t->preempt = 1;
        }
        if (all) { ok = true; break; }
        const uint64_t now = now_us();
        if (now - t0 > (uint64_t)timeout_ms * 1000) break;
        if (now - last_kick > 2000) {   // wake the keyed waiters and core waiters so they reach their gate
            for (uint64_t tt : tts)
                if (Thread *t = find_tt(tt)) t->cv.notify_all();
            last_kick = now;
        }
        g_coord.wait_for(lk, std::chrono::microseconds(500));
    }
    drain_ms = (now_us() - t0) / 1000.0;
    for (uint64_t tt : tts) {
        Thread *t = find_tt(tt);
        if (!t || t->exited) continue;
        Row r;
        r.name = tname(t);
        r.id = t->id;
        r.prio = t->eff_prio;
        r.wkind = t->wkind;
        r.core = t->core;
        r.unclean = t->unclean;
        r.on_fiber = t->cur_fiber != nullptr;
        r.host_blocked = t->block_depth > 0;
        r.site = SITE_NONE;
        r.park_ms = -1;
        auto it = g_park.find(t);
        if (it != g_park.end() && it->second.round == g_round) {
            r.site = it->second.site;
            r.park_ms = (it->second.t_us - t0) / 1000.0;
            r.fr.assign(it->second.fr, it->second.fr + it->second.n);
        }
        rows.push_back(std::move(r));
    }
    return ok;
}

void release_all() {
    std::vector<uint64_t> tts = all_thread_types();
    Lock lk(G);
    g_sphase.store(0);
    for (uint64_t tt : tts)
        if (Thread *t = find_tt(tt)) t->cv.notify_all();
}

void census_driver(int start_s, int rounds, int gap_s) {
    Sleep(start_s * 1000);
    g_sym.load();
    for (int i = 0; i < rounds; i++) {
        std::vector<Row> rows;
        double ms = 0;
        std::string last;
        OsStats st;
        os_get_stats(&st);
        const bool ok = drain(2000, rows, ms, last);
        uint64_t mem[6] = {};
        snap_os_mem_stats(mem);
        log("state census %d/%d: %s in %.1f ms; threads started %llu exited %llu fiber_switches %llu; vamm committed %llu MiB heap %llu MiB (%llu blocks)%s%s",
            i + 1, rounds, ok ? "all parked" : "TIMEOUT", ms, (unsigned long long)st.threads_started, (unsigned long long)st.threads_exited,
            (unsigned long long)st.fiber_switches, (unsigned long long)(mem[1] >> 20), (unsigned long long)(mem[3] >> 20),
            (unsigned long long)mem[4], ok ? "" : " waiting for: ", ok ? "" : last.c_str());
        for (const Row &r : rows) {
            std::string line;
            for (void *a : r.fr) {
                std::string s = g_sym.name(a);
                if (boring(s)) continue;
                if (!line.empty()) line += " < ";
                line += s;
                if (line.size() > 700) { line += " ..."; break; }
            }
            log("  thr id%llu %-22.22s p%-2d %s wkind=%d core=%d%s%s%s park=%.1fms | %s", (unsigned long long)r.id, r.name.c_str(), r.prio,
                r.site == SITE_WAIT ? "WAIT" : r.site == SITE_ACQ ? "ACQ " : "----", r.wkind, r.core, r.host_blocked ? " hostblk" : "",
                r.unclean ? " UNCLEAN" : "", r.on_fiber ? " FIBER" : "", r.park_ms, line.c_str());
        }
        release_all();
        Sleep(gap_s * 1000);
    }
}
}  // namespace

void state_gate_wait(Thread *t, Lock &lk) {
    (void)lk;
    if (g_sphase.load(std::memory_order_relaxed) == 1) note(t, SITE_WAIT);
}

bool state_gate_acq(Thread *t, Lock &lk) {
    if (g_sphase.load(std::memory_order_relaxed) != 1) return false;
    if (t->is_host || t->free_core) return false;
    if (t->unclean > 0) {   // host frames above may own heap objects: grant once, the thread parks at its next check-in
        g_unclean_grants++;
        return false;
    }
    note(t, SITE_ACQ);
    t->sgated = true;
    state_kick_cores();   // a thread that waits for a core after an unclean grant may have been blocked by this waiter
    t->cv.wait(lk);
    t->sgated = false;
    return true;
}

void state_init() {
    const char *e = getenv("SNAP_STATE_TEST");
    if (!e || strncmp(e, "census@t", 8) != 0) return;
    int start = atoi(e + 8), rounds = 5, gap = 2;
    if (const char *p = strstr(e, ",n")) rounds = atoi(p + 2);
    if (const char *p = strstr(e, ",gap")) gap = atoi(p + 4);
    std::thread(census_driver, start, rounds, gap).detach();
}

}  // namespace osk
