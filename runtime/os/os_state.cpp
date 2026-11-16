// Save states, part 1: drain the virtual cores and park every guest thread at a clean point (docs/save-states.md section 2.2).
// This file holds the drain/park machinery and the M1 census (SNAP_STATE_TEST=census@t<seconds>[,n<rounds>][,gap<seconds>]).
// Copy, capture and restore build on it (os_state_snap.cpp). Game behavior is unchanged while g_sphase is 0.
#include "os_internal.h"
#include "os_layout.h"
#include <windows.h>
#include <dbghelp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" void snap_os_mem_stats(uint64_t *out);

// Host objects the OS layer cannot see (the NVN layer registers these from runtime/nvn/nvn_state.cpp; unset in the standalone OS test).
extern "C" {
struct SnapStateHooks {
    void (*pre_save)();
    int (*pre_load)(char *why, int cap);
    void (*post_load)();
    void (*shot)();   // test aid: request a screenshot
};
}
static const SnapStateHooks *g_xh = nullptr;
extern "C" void snap_state_set_hooks(const SnapStateHooks *h) { g_xh = h; }

// Requests from the HLE layer (route tokens), SNAP_CHECKPOINT and the player's buttons. op 1 = save, 2 = load. They only queue; one worker thread does the drain.
extern "C" {
struct SnapStateInfo {
    int busy, has_state, last_op, last_ok, saves, loads, done;
    unsigned long long last_done_ms, saved_ms;   // GetTickCount64-style ms of the last finished request / the last good save
    char last_msg[112];
};
void snap_state_request(int op, const char *name);
int snap_state_idle();
int snap_state_count(const char *kind);
void snap_state_info(SnapStateInfo *out);
}

using namespace oslay;

namespace osk {

std::atomic<int> g_sphase{0};   // 0 idle, 1 drain: no thread is granted a core at a clean site
using StateInfo = ::SnapStateInfo;
void state_request(int op, const char *name);
bool state_idle();
uint64_t g_process_start_ms = 0;

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

// ======================================================================================================================
// Save states, part 2 (M3): capture the parked threads and the runtime state, put it all back later.
//
// A parked thread stays inside gate_block (below) for the whole freeze. To save, each thread captures itself there: its callee-saved
// registers (snap_capture) and the bytes of its current host stack from the capture point up (copy_out). To load, each thread moves to
// a small side stack, lets go of G, waits for the coordinator to put the memory and the records back, copies its saved stack bytes
// over its real stack, takes G again and resumes into the saved registers: it is back inside gate_block, as it was when saved.
// ======================================================================================================================
extern "C" {
int snap_capture(void *regs) __attribute__((returns_twice));
void snap_resume(void *regs) __attribute__((noreturn));
void snap_call_on_stack(void (*fn)(void *), void *arg, void *top) __attribute__((noreturn));
}
// Regs layout (bytes): rbx 0, rbp 8, rdi 16, rsi 24, r12 32, r13 40, r14 48, r15 56, rsp 64 (after the return), return address 72,
// mxcsr 80, x87 control word 84, xmm6-15 at 96 + 16 * i. Total 256.
__asm__(
    ".text\n"
    ".globl snap_capture\n"
    "snap_capture:\n"
    "  movq %rbx, 0(%rcx)\n  movq %rbp, 8(%rcx)\n  movq %rdi, 16(%rcx)\n  movq %rsi, 24(%rcx)\n"
    "  movq %r12, 32(%rcx)\n  movq %r13, 40(%rcx)\n  movq %r14, 48(%rcx)\n  movq %r15, 56(%rcx)\n"
    "  leaq 8(%rsp), %rax\n  movq %rax, 64(%rcx)\n"
    "  movq (%rsp), %rax\n  movq %rax, 72(%rcx)\n"
    "  stmxcsr 80(%rcx)\n  fnstcw 84(%rcx)\n"
    "  movups %xmm6, 96(%rcx)\n  movups %xmm7, 112(%rcx)\n  movups %xmm8, 128(%rcx)\n  movups %xmm9, 144(%rcx)\n  movups %xmm10, 160(%rcx)\n"
    "  movups %xmm11, 176(%rcx)\n  movups %xmm12, 192(%rcx)\n  movups %xmm13, 208(%rcx)\n  movups %xmm14, 224(%rcx)\n  movups %xmm15, 240(%rcx)\n"
    "  xorl %eax, %eax\n"
    "  ret\n"
    ".globl snap_resume\n"
    "snap_resume:\n"
    "  movq 0(%rcx), %rbx\n  movq 8(%rcx), %rbp\n  movq 16(%rcx), %rdi\n  movq 24(%rcx), %rsi\n"
    "  movq 32(%rcx), %r12\n  movq 40(%rcx), %r13\n  movq 48(%rcx), %r14\n  movq 56(%rcx), %r15\n"
    "  ldmxcsr 80(%rcx)\n  fldcw 84(%rcx)\n"
    "  movups 96(%rcx), %xmm6\n  movups 112(%rcx), %xmm7\n  movups 128(%rcx), %xmm8\n  movups 144(%rcx), %xmm9\n  movups 160(%rcx), %xmm10\n"
    "  movups 176(%rcx), %xmm11\n  movups 192(%rcx), %xmm12\n  movups 208(%rcx), %xmm13\n  movups 224(%rcx), %xmm14\n  movups 240(%rcx), %xmm15\n"
    "  movq 72(%rcx), %rdx\n  movq 64(%rcx), %rsp\n  movl $1, %eax\n  jmpq *%rdx\n"
    ".globl snap_call_on_stack\n"
    "snap_call_on_stack:\n"
    "  movq %rcx, %rax\n  movq %rdx, %rcx\n  movq %r8, %rsp\n  subq $32, %rsp\n  callq *%rax\n  int3\n");

namespace {
enum { PH_IDLE = 0, PH_DRAIN = 1, PH_CAPTURE = 2, PH_RESTORE = 3 };

inline uint64_t gs_read(uint32_t off) {
    uint64_t v;
    __asm__ volatile("movq %%gs:(%1), %0" : "=r"(v) : "r"((uint64_t)off));
    return v;
}
inline void gs_write(uint32_t off, uint64_t v) { __asm__ volatile("movq %0, %%gs:(%1)" ::"r"(v), "r"((uint64_t)off) : "memory"); }

void *big_alloc(size_t n) { return VirtualAlloc(nullptr, n ? n : 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }
void big_free(void *p) { if (p) VirtualFree(p, 0, MEM_RELEASE); }

struct TRec {   // the scheduling and wait fields of a Thread (under G)
    Cpu *cur = nullptr;
    int block_depth = 0, base_prio = 0, eff_prio = 0, ideal = 0, core = -1, last_core = 0, joiners = 0, wkind = 0, unclean = 0;
    uint64_t mask = 0, core_since = 0, ready_seq = 0, wkey = 0, waddr = 0, wseq = 0, own_fiber = 0;
    uint32_t wtag = 0, wresult = 0;
    bool waiting_core = false, core_kicked = false, woken = false, sgated = false;
    Thread *lock_owner = nullptr;
    std::vector<Thread *> lock_waiters;
    HostCtx *base_fiber = nullptr;
    uint64_t base_serial = 0;
    Fiber *own_shadow = nullptr, *cur_fiber = nullptr, *leaving = nullptr;
    void save(const Thread *t) {
        cur = t->cur; block_depth = t->block_depth; base_prio = t->base_prio; eff_prio = t->eff_prio; ideal = t->ideal; core = t->core;
        last_core = t->last_core; joiners = t->joiners; wkind = t->wkind; unclean = t->unclean; mask = t->mask; core_since = t->core_since;
        ready_seq = t->ready_seq; wkey = t->wkey; waddr = t->waddr; wseq = t->wseq; own_fiber = t->own_fiber; wtag = t->wtag;
        wresult = t->wresult; waiting_core = t->waiting_core; core_kicked = t->core_kicked; woken = t->woken; sgated = t->sgated;
        lock_owner = t->lock_owner; lock_waiters = t->lock_waiters; base_fiber = t->base_fiber; base_serial = t->base_fiber ? t->base_fiber->serial : 0;
        own_shadow = t->own_shadow; cur_fiber = t->cur_fiber; leaving = t->leaving;
    }
    void restore(Thread *t) const {
        t->cur = cur; t->block_depth = block_depth; t->base_prio = base_prio; t->eff_prio = eff_prio; t->ideal = ideal; t->core = core;
        t->last_core = last_core; t->joiners = joiners; t->wkind = wkind; t->unclean = unclean; t->mask = mask; t->core_since = core_since;
        t->ready_seq = ready_seq; t->wkey = wkey; t->waddr = waddr; t->wseq = wseq; t->own_fiber = own_fiber; t->wtag = wtag;
        t->wresult = wresult; t->waiting_core = waiting_core; t->core_kicked = core_kicked; t->woken = woken; t->sgated = sgated;
        t->lock_owner = lock_owner; t->lock_waiters = lock_waiters;
        if (t->base_fiber && t->base_fiber != base_fiber) ctx_free(t->base_fiber);   // created after the save: leave no stack behind
        t->base_fiber = base_fiber;
        t->own_shadow = own_shadow; t->cur_fiber = cur_fiber; t->leaving = leaving;
        t->preempt.store(0);
        t->core_wait_since = now_us();
        t->humble_until = 0;
    }
};

struct TState {   // one per guest thread in the saved state
    Thread *t = nullptr;
    uint64_t cap_gen = 0, rest_gen = 0;
    alignas(16) uint8_t regs[256];
    uint8_t *lo = nullptr;       // regs.rsp: the saved stack bytes belong at [lo, lo + n)
    uint8_t *bytes = nullptr;
    size_t n = 0, cap = 0;
    uint64_t tib_base = 0, tib_limit = 0, tib_dealloc = 0;
    uint8_t *side = nullptr;     // side stack for the restore
    std::atomic<int> arrived{0}, copied{0}, resumed{0};
    TRec rec;
};

struct FRec {   // a fiber, or a thread's own context, as far as the host stack is concerned
    Fiber *f = nullptr;
    uint64_t guest = 0;
    Cpu *cpu = nullptr;
    HostCtx *host = nullptr;
    uint64_t serial = 0;
    void *sp = nullptr;
    bool finished = false, is_thread_ctx = false, running = false;
    uint8_t *bytes = nullptr;    // [sp, base) while the context is suspended
    size_t n = 0;
};

struct Rng { uint64_t base = 0, size = 0; uint8_t *buf = nullptr; };

struct Slot {
    bool valid = false;
    std::deque<TState> ts;
    std::unordered_map<Thread *, TState *> by_thread;
    SchedSnap sched;
    SyncSnap sync;
    MemBook mem;
    int64_t time = 0;
    std::vector<Rng> ranges;
    std::vector<FRec> frecs;
    std::vector<std::pair<uint64_t, Fiber *>> fibers;
    double save_ms = 0;
    size_t bytes_mem = 0, bytes_stack = 0;
};

Slot g_slot;
std::atomic<int> g_go1{0}, g_go2{0};
uint64_t g_cap_gen = 0, g_rest_gen = 0;   // under G

TState *ts_of(Thread *t) {   // under G
    if (!g_slot.valid && g_slot.by_thread.empty()) return nullptr;
    auto it = g_slot.by_thread.find(t);
    return it == g_slot.by_thread.end() ? nullptr : it->second;
}

__attribute__((noinline)) void copy_out(TState *ts) {
    uint8_t *lo = (uint8_t *)(uintptr_t) * (uint64_t *)(ts->regs + 64);
    ts->tib_base = gs_read(8);
    ts->tib_limit = gs_read(16);
    ts->tib_dealloc = gs_read(0x1478);
    const size_t n = (size_t)(ts->tib_base - (uint64_t)(uintptr_t)lo);
    if (n > (64u << 20)) fatal("save state: thread %#llx host stack use %zu bytes", (unsigned long long)ts->t->id, n);
    if (n > ts->cap) {
        big_free(ts->bytes);
        ts->bytes = (uint8_t *)big_alloc(n + 4096);
        ts->cap = n + 4096;
    }
    memcpy(ts->bytes, lo, n);
    ts->lo = lo;
    ts->n = n;
}

__attribute__((noinline)) void capture_point(TState *ts) {
    if (snap_capture(ts->regs) == 0) {
        copy_out(ts);
        ts->cap_gen = g_cap_gen;
        g_coord.notify_all();
    } else {   // back here after a load: the stack and registers are the saved ones, G is held
        ts->resumed.fetch_add(1);
        g_coord.notify_all();
    }
}

void restore_stage(void *p) {   // runs on the side stack, G held on entry
    TState *ts = (TState *)p;
    ts->rest_gen = g_rest_gen;
    G.unlock();
    ts->arrived.store(1);
    while (!g_go1.load()) Sleep(1);
    memcpy(ts->lo, ts->bytes, ts->n);
    ts->copied.store(1);
    while (!g_go2.load()) Sleep(1);
    G.lock();
    gs_write(8, ts->tib_base);
    gs_write(16, ts->tib_limit);
    gs_write(0x1478, ts->tib_dealloc);
    snap_resume(ts->regs);
}

void gate_block(Thread *t, Lock &lk, int site) {
    note(t, site);
    for (;;) {
        const int ph = g_sphase.load();
        if (ph == PH_IDLE) return;
        if (ph == PH_CAPTURE || ph == PH_RESTORE) {
            if (TState *ts = ts_of(t)) {
                if (ph == PH_CAPTURE && ts->cap_gen != g_cap_gen) capture_point(ts);
                else if (ph == PH_RESTORE && ts->rest_gen != g_rest_gen) snap_call_on_stack(restore_stage, ts, ts->side + (64 << 10) - 64);
            }
        }
        t->cv.wait(lk);
    }
}

// ---- memory ranges
bool writable(DWORD prot) {
    if (prot & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return prot & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
}
void add_committed(std::vector<Rng> &out, uint64_t lo, uint64_t hi) {   // the committed writable pages inside [lo, hi)
    uint64_t a = lo;
    while (a < hi) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
        const uint64_t b = (uint64_t)(uintptr_t)mi.BaseAddress, e = std::min<uint64_t>(b + mi.RegionSize, hi);
        if (mi.State == MEM_COMMIT && writable(mi.Protect)) out.push_back(Rng{(std::max)(a, b), e - (std::max)(a, b), nullptr});
        if (e <= a) break;
        a = e;
    }
}
void add_exact(std::vector<Rng> &out, uint64_t a, uint64_t n) {   // a small host object: only if it sits in committed writable memory
    if (!a || !n) return;
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) && mi.State == MEM_COMMIT && writable(mi.Protect) && a + n <= (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize)
        out.push_back(Rng{a, n, nullptr});
}
void merge(std::vector<Rng> &v) {
    std::sort(v.begin(), v.end(), [](const Rng &a, const Rng &b) { return a.base < b.base; });
    std::vector<Rng> o;
    for (const Rng &r : v) {
        if (!o.empty() && r.base <= o.back().base + o.back().size) o.back().size = (std::max)(o.back().base + o.back().size, r.base + r.size) - o.back().base;
        else o.push_back(r);
    }
    v.swap(o);
}

std::vector<Rng> enumerate(const MemBook &mem, const std::vector<Thread *> &threads, const std::vector<std::pair<uint64_t, Fiber *>> &fibers) {
    std::vector<Rng> v;
    if (mem.heap_base) add_committed(v, mem.heap_base, mem.heap_base + mem.heap_size);
    for (auto &kv : mem.regions) add_committed(v, kv.first, kv.first + kv.second.first);
    add_committed(v, 0x7100000000ull, 0x7100000000ull + 0x4000000);   // main module (host/loader.h GUEST_MAIN_BASE)
    add_committed(v, 0x7110000000ull, 0x7110000000ull + 0x2000000);   // sdk module
    for (Thread *t : threads) {
        add_exact(v, (uint64_t)(uintptr_t)t->tlr, TLR_SIZE);
        add_exact(v, t->tt, TT_SIZE);
        add_exact(v, t->own_fiber, FB_SIZE);
        add_exact(v, (uint64_t)(uintptr_t)t->cpu, sizeof(Cpu));
        const uint64_t st = gld<uint64_t>(t->tt + TT_STACK), ss = gld<uint64_t>(t->tt + TT_STACK_SIZE);   // the loader's main thread owns its stack and TLS
        if (st && ss && ss < (1ull << 30)) add_committed(v, st, st + ss);
        if (t->cpu && t->cpu->tpidr) {
            const uint64_t tls = t->cpu->tpidr - 0x8000;
            MEMORY_BASIC_INFORMATION mi;
            if (VirtualQuery((void *)(uintptr_t)tls, &mi, sizeof mi) && (uint64_t)(uintptr_t)mi.AllocationBase == tls && mi.RegionSize == 0x10000)
                add_committed(v, tls, tls + 0x10000);
        }
    }
    for (auto &fk : fibers) add_exact(v, (uint64_t)(uintptr_t)fk.second->cpu, sizeof(Cpu));
    merge(v);
    return v;
}

std::vector<uint64_t> g_tts;   // all_thread_types() takes G itself: read it before locking, use it under G

std::vector<Thread *> guest_threads() {   // under G: started, not exited, not a host helper
    std::vector<Thread *> out;
    for (uint64_t tt : g_tts) {
        Thread *t = find_tt(tt);
        if (t && !t->is_host && t->started && !t->exited) out.push_back(t);
    }
    return out;
}
std::vector<Thread *> every_thread() {   // under G: guest threads and host helpers, for their guest-visible objects
    std::vector<Thread *> out;
    for (uint64_t tt : g_tts)
        if (Thread *t = find_tt(tt)) out.push_back(t);
    return out;
}

void slot_free() {
    for (Rng &r : g_slot.ranges) big_free(r.buf);
    for (FRec &f : g_slot.frecs) big_free(f.bytes);
    for (TState &s : g_slot.ts) { big_free(s.bytes); big_free(s.side); }
    g_slot.ranges.clear();
    g_slot.frecs.clear();
    g_slot.ts.clear();
    g_slot.by_thread.clear();
    g_slot.valid = false;
}

void copy_suspended(FRec &r) {
    if (!r.host || r.running || !r.host->sp) return;
    const uint8_t *lo = (const uint8_t *)r.host->sp, *hi = (const uint8_t *)r.host->base;
    if (hi <= lo || (size_t)(hi - lo) > (64u << 20)) fatal("save state: suspended context %#llx has sp %p base %p", (unsigned long long)r.guest, r.host->sp, r.host->base);
    r.n = (size_t)(hi - lo);
    r.bytes = (uint8_t *)big_alloc(r.n);
    memcpy(r.bytes, lo, r.n);
}

bool do_save(std::string &why) {
    const uint64_t t0 = now_us();
    std::vector<Row> rows;
    double dms = 0;
    std::string last;
    if (!drain(2000, rows, dms, last)) { why = "drain timed out waiting for " + last; release_all(); return false; }
    g_tts = all_thread_types();
    Lock lk(G);
    slot_free();
    std::vector<Thread *> gt = guest_threads();
    for (Thread *t : gt) {
        if (t->block_depth > 0 || t->core >= 0) { why = "a thread still holds a core or a host block"; g_sphase.store(PH_IDLE); for (Thread *x : every_thread()) x->cv.notify_all(); return false; }
        g_slot.ts.emplace_back();
        TState &s = g_slot.ts.back();
        s.t = t;
        s.side = (uint8_t *)big_alloc(64 << 10);
        g_slot.by_thread[t] = &s;
    }
    if (g_xh && g_xh->pre_save) g_xh->pre_save();   // GPU idle, readbacks landed in guest memory, NVN snapshot (before any memory is copied)
    g_cap_gen++;
    g_sphase.store(PH_CAPTURE);
    for (Thread *t : every_thread()) t->cv.notify_all();
    const uint64_t t1 = now_us();
    for (;;) {
        bool all = true;
        for (TState &s : g_slot.ts)
            if (s.cap_gen != g_cap_gen) all = false;
        if (all) break;
        if (now_us() - t1 > 3000000) { why = "capture timed out"; break; }
        for (Thread *t : every_thread()) t->cv.notify_all();
        g_coord.wait_for(lk, std::chrono::milliseconds(2));
    }
    bool ok = why.empty();
    if (ok) {
        std::vector<Thread *> all_t = every_thread();
        for (TState &s : g_slot.ts) { s.rec.save(s.t); g_slot.bytes_stack += s.n; }
        state_sched_save(g_slot.sched);
        state_sync_save(g_slot.sync);
        state_mem_save(g_slot.mem);
        g_slot.fibers = state_fibers();
        auto add_f = [&](Fiber *f) {
            FRec r;
            r.f = f; r.guest = f->guest; r.cpu = f->cpu; r.host = f->host; r.serial = f->host ? f->host->serial : 0; r.sp = f->host ? f->host->sp : nullptr;
            r.finished = f->finished; r.is_thread_ctx = f->is_thread_ctx; r.running = f->running.load();
            copy_suspended(r);
            g_slot.bytes_stack += r.n;
            g_slot.frecs.push_back(r);
        };
        for (auto &fk : g_slot.fibers) add_f(fk.second);
        for (Thread *t : all_t)
            if (t->own_shadow) add_f(t->own_shadow);
        g_slot.ranges = enumerate(g_slot.mem, all_t, g_slot.fibers);
        for (Rng &r : g_slot.ranges) {
            r.buf = (uint8_t *)big_alloc(r.size);
            memcpy(r.buf, (const void *)(uintptr_t)r.base, r.size);
            g_slot.bytes_mem += r.size;
        }
        g_slot.time = state_time_get();
        g_slot.valid = true;
    }
    g_sphase.store(PH_IDLE);
    for (Thread *t : every_thread()) t->cv.notify_all();
    g_slot.save_ms = (now_us() - t0) / 1000.0;
    if (!ok) slot_free();
    return ok;
}

bool writable_now(const Rng &r) {
    uint64_t a = r.base;
    while (a < r.base + r.size) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) || mi.State != MEM_COMMIT || !writable(mi.Protect)) return false;
        a = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
    }
    return true;
}

// Checks, under G with every thread parked, that what the save saw is still there. Returns an empty string when a load can go ahead.
std::string load_refusal() {
    if (!g_slot.valid) return "no saved state";
    std::vector<Thread *> gt = guest_threads();
    if (gt.size() != g_slot.ts.size()) return "thread count changed (" + std::to_string(g_slot.ts.size()) + " -> " + std::to_string(gt.size()) + ")";
    for (Thread *t : gt)
        if (!g_slot.by_thread.count(t)) return "a thread was created since the save";
    for (TState &s : g_slot.ts)
        if (s.t->exited || !s.t->started) return "a thread exited since the save";
    SchedSnap now;
    state_sched_save(now);
    if (now.by_tt.size() != g_slot.sched.by_tt.size()) return "thread table changed";
    if (state_fibers() != g_slot.fibers) return "the fiber set changed";
    for (FRec &r : g_slot.frecs) {
        const uint64_t cur = r.f->host ? r.f->host->serial : 0;
        if (r.host && cur != r.serial) return "a fiber stack was re-created";
    }
    std::string m = state_mem_check(g_slot.mem);   // regions made since the save are released by the load; regions released since must have free addresses
    if (!m.empty()) return m;
    MemBook now_mem;
    state_mem_save(now_mem);
    for (Rng &r : g_slot.ranges) {
        if (writable_now(r)) continue;
        bool released = false;   // inside a VAMM region that was released since: the load re-reserves and recommits it
        for (auto &kv : g_slot.mem.regions)
            if (r.base >= kv.first && r.base + r.size <= kv.first + kv.second.first && !now_mem.regions.count(kv.first)) released = true;
        if (!released) return "memory at " + std::to_string(r.base) + " is no longer committed";
    }
    return "";
}

bool do_load(std::string &why) {
    const uint64_t t0 = now_us();
    if (!g_slot.valid) { why = "no saved state"; return false; }
    std::vector<Row> rows;
    double dms = 0;
    std::string last;
    if (!drain(2000, rows, dms, last)) { why = "drain timed out waiting for " + last; release_all(); return false; }
    if (g_xh && g_xh->pre_load) {   // GPU idle and readbacks landed BEFORE guest memory goes back, then the NVN object sets must still match
        char buf[160] = {};
        if (!g_xh->pre_load(buf, sizeof buf)) { why = buf; release_all(); return false; }
    }
    g_tts = all_thread_types();
    {
        Lock lk(G);
        why = load_refusal();
        if (!why.empty()) {
            g_sphase.store(PH_IDLE);
            for (Thread *t : every_thread()) t->cv.notify_all();
            return false;
        }
        for (TState &s : g_slot.ts) { s.arrived = 0; s.copied = 0; s.resumed = 0; }
        g_go1 = 0;
        g_go2 = 0;
        g_rest_gen++;
        g_sphase.store(PH_RESTORE);
        for (Thread *t : every_thread()) t->cv.notify_all();
    }
    const uint64_t t1 = now_us();
    auto all_flag = [&](std::atomic<int> TState::*m) {
        for (TState &s : g_slot.ts)
            if (!(s.*m).load()) return false;
        return true;
    };
    while (!all_flag(&TState::arrived)) {
        if (now_us() - t1 > 5000000) fatal("load state: threads did not reach the side stacks (%llu ms)", (unsigned long long)((now_us() - t1) / 1000));
        Sleep(0);
        Lock lk(G);
        for (Thread *t : every_thread()) t->cv.notify_all();
        lk.unlock();
        Sleep(1);
    }
    {
        Lock lk(G);   // every guest thread is on a side stack with G released; put the world back
        const std::string mr = state_mem_restore(g_slot.mem);
        if (!mr.empty()) fatal("load state: %s", mr.c_str());   // load_refusal checked the addresses; the threads are already on side stacks
        for (Rng &r : g_slot.ranges) {
            if (!writable_now(r) && !VirtualAlloc((void *)(uintptr_t)r.base, r.size, MEM_COMMIT, PAGE_READWRITE))
                fatal("load state: cannot recommit %llu bytes at %llx", (unsigned long long)r.size, (unsigned long long)r.base);
            memcpy((void *)(uintptr_t)r.base, r.buf, r.size);
        }
        state_sched_restore(g_slot.sched);
        state_sync_restore(g_slot.sync);
        for (TState &s : g_slot.ts) s.rec.restore(s.t);
        for (FRec &r : g_slot.frecs) {
            Fiber *f = r.f;
            f->host = r.host;
            f->finished = r.finished;
            f->running.store(r.running);
            if (r.host) r.host->sp = r.sp;
            if (r.bytes) memcpy(r.host->sp, r.bytes, r.n);
        }
        state_time_set(g_slot.time);
        if (g_xh && g_xh->post_load) g_xh->post_load();   // host objects made since the save go, zombies come back, guest-API fields are restored
        g_go1 = 1;
    }
    while (!all_flag(&TState::copied)) Sleep(1);
    g_go2 = 1;
    while (!all_flag(&TState::resumed)) {
        if (now_us() - t1 > 15000000) fatal("load state: threads did not resume");
        Sleep(1);
    }
    release_all();
    why = std::to_string((now_us() - t0) / 1000.0);
    return true;
}

size_t private_mib() {
    struct PMC { DWORD cb, faults; SIZE_T peakws, ws, qpp, qp, qpnp, qnp, pagefile, peakpagefile; } m = {};
    m.cb = sizeof m;
    static auto fn = (BOOL(WINAPI *)(HANDLE, void *, DWORD))GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
    if (!fn || !fn(GetCurrentProcess(), &m, sizeof m)) return 0;
    return m.pagefile >> 20;
}

// ",shots": a screenshot 1.5 s after the save and after every load, so the frames at the same guest moment can be compared (SNAP_SHOT_DIR)
struct TestPlan { int save_s = -1, load_s = -1, loops = 1, lgap = 3; bool shots = false; };
void take_shot() {
    if (g_xh && g_xh->shot) g_xh->shot();
    Sleep(500);
}

void save_load_driver(TestPlan p) {
    Sleep(p.save_s * 1000);
    std::string why;
    bool ok = do_save(why);
    log("state save: %s in %.0f ms: %zu threads, %zu MiB guest memory in %zu ranges, %zu KiB host stack; process commit %zu MiB%s%s", ok ? "ok" : "REFUSED", g_slot.save_ms,
        g_slot.ts.size(), g_slot.bytes_mem >> 20, g_slot.ranges.size(), g_slot.bytes_stack >> 10, private_mib(), ok ? "" : ": ", ok ? "" : why.c_str());
    if (!ok || p.load_s < 0) return;
    const uint64_t saved_at = now_us();
    if (p.shots) { Sleep(1500); take_shot(); log("state shot A (save + 1.5 s)"); }
    Sleep((DWORD)(std::max)(0, (p.load_s - p.save_s) * 1000 - (int)((now_us() - saved_at) / 1000)));
    for (int i = 0; i < p.loops; i++) {
        if (i) Sleep(p.lgap * 1000);
        std::string w;
        const uint64_t a = now_us();
        bool lok = do_load(w);
        log("state load %d/%d: %s %s ms%s; process commit %zu MiB", i + 1, p.loops, lok ? "ok" : "REFUSED", lok ? w.c_str() : "-", lok ? "" : (": " + w).c_str(), private_mib());
        (void)a;
        if (p.shots && lok) { Sleep(1500); take_shot(); log("state shot B%d (load + 1.5 s)", i + 1); }
    }
}

// ---- requests from route scripts, SNAP_CHECKPOINT and the player's buttons -------------------------------------------------------------
// A save or load drains every guest thread, so it can never run on one: requests go to one worker thread. Save and load return at once;
// snap_state_idle() says when the worker is done (route condition `state=idle`).
enum { OP_SAVE = 1, OP_LOAD = 2 };
struct Req { int op; std::string name; };
std::mutex g_rq_mu;
std::condition_variable g_rq_cv;
std::deque<Req> g_rq;
bool g_rq_started = false;
std::atomic<int> g_rq_busy{0};
std::string g_slot_name;
std::mutex g_info_mu;
StateInfo g_info;

void run_request(const Req &r) {
    std::string why;
    if (r.op == OP_SAVE) {
        const bool ok = do_save(why);
        log("state save '%s': %s in %.0f ms: %zu threads, %zu MiB guest memory in %zu ranges, %zu KiB host stack; process commit %zu MiB%s%s", r.name.c_str(), ok ? "ok" : "REFUSED",
            g_slot.save_ms, g_slot.ts.size(), g_slot.bytes_mem >> 20, g_slot.ranges.size(), g_slot.bytes_stack >> 10, private_mib(), ok ? "" : ": ", ok ? "" : why.c_str());
        std::lock_guard<std::mutex> lk(g_info_mu);
        if (ok) { g_slot_name = r.name; g_info.has_state = true; g_info.saves++; g_info.saved_ms = GetTickCount64(); }
        g_info.done++; g_info.last_op = OP_SAVE; g_info.last_ok = ok; g_info.last_done_ms = GetTickCount64();
        snprintf(g_info.last_msg, sizeof g_info.last_msg, "%s", ok ? "state saved" : why.c_str());
        if (!ok && !g_slot.valid) g_info.has_state = false;
    } else {
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(g_info_mu);
            if (!g_info.has_state) why = "no saved state";
            else if (!r.name.empty() && r.name != g_slot_name) why = "no state named '" + r.name + "' (the slot holds '" + g_slot_name + "')";
            else why = "ok";
            ok = why == "ok";
        }
        if (ok) { why.clear(); ok = do_load(why); }
        if (ok) log("state load '%s': ok %s ms; process commit %zu MiB", r.name.c_str(), why.c_str(), private_mib());
        else log("state load '%s': REFUSED: %s; process commit %zu MiB", r.name.c_str(), why.c_str(), private_mib());
        std::lock_guard<std::mutex> lk(g_info_mu);
        if (ok) g_info.loads++;
        g_info.done++; g_info.last_op = OP_LOAD; g_info.last_ok = ok; g_info.last_done_ms = GetTickCount64();
        snprintf(g_info.last_msg, sizeof g_info.last_msg, "%s", ok ? "state loaded" : why.c_str());
    }
}

void request_worker() {
    for (;;) {
        Req r;
        {
            std::unique_lock<std::mutex> lk(g_rq_mu);
            g_rq_cv.wait(lk, [] { return !g_rq.empty(); });
            r = g_rq.front();
            g_rq.pop_front();
            g_rq_busy = 1;
        }
        run_request(r);
        std::lock_guard<std::mutex> lk(g_rq_mu);
        g_rq_busy = 0;
    }
}

// SNAP_CHECKPOINT=save:<name>@t<s>,load:<name>@t<s>[x<n>],... timed actions, seconds from process start (x<n>: repeat n times, 4 s apart)
void checkpoint_driver(std::string spec) {
    struct Act { int op; std::string name; int at; int times; };
    std::vector<Act> acts;
    size_t i = 0;
    while (i < spec.size()) {
        size_t j = spec.find(',', i);
        if (j == std::string::npos) j = spec.size();
        std::string t = spec.substr(i, j - i);
        i = j + 1;
        const bool sv = t.rfind("save:", 0) == 0, ld = t.rfind("load:", 0) == 0;
        if (!sv && !ld) continue;
        Act a{sv ? OP_SAVE : OP_LOAD, "", 0, 1};
        size_t at = t.find("@t");
        a.name = t.substr(5, at == std::string::npos ? std::string::npos : at - 5);
        if (at != std::string::npos) {
            a.at = atoi(t.c_str() + at + 2);
            size_t x = t.find('x', at);
            if (x != std::string::npos) a.times = (std::max)(1, atoi(t.c_str() + x + 1));
        }
        acts.push_back(a);
    }
    std::stable_sort(acts.begin(), acts.end(), [](const Act &a, const Act &b) { return a.at < b.at; });
    for (const Act &a : acts)
        for (int n = 0; n < a.times; n++) {
            const int due_ms = (a.at + n * 4) * 1000;
            const uint64_t up = GetTickCount64() - g_process_start_ms;
            if (up < (uint64_t)due_ms) Sleep((DWORD)(due_ms - up));
            state_request(a.op, a.name.c_str());
            while (!state_idle()) Sleep(20);
        }
}
}  // namespace

void state_request(int op, const char *name) {
    {
        std::lock_guard<std::mutex> lk(g_rq_mu);
        g_rq.push_back({op, name ? name : ""});
        if (!g_rq_started) { g_rq_started = true; std::thread(request_worker).detach(); }
    }
    g_rq_cv.notify_one();
}

bool state_idle() {
    std::lock_guard<std::mutex> lk(g_rq_mu);
    return g_rq.empty() && !g_rq_busy.load();
}

StateInfo state_info() {
    std::lock_guard<std::mutex> lk(g_info_mu);
    StateInfo i = g_info;
    i.busy = !state_idle();
    return i;
}

void state_gate_wait(Thread *t, Lock &lk) {
    if (g_sphase.load(std::memory_order_relaxed) == 0) return;
    gate_block(t, lk, SITE_WAIT);
}

bool state_gate_acq(Thread *t, Lock &lk) {
    const int ph = g_sphase.load(std::memory_order_relaxed);
    if (ph == 0) return false;
    if (t->is_host || t->free_core) return false;
    if (t->unclean > 0 && ph == PH_DRAIN) {   // host frames above may own heap objects: grant once, the thread parks at its next check-in
        g_unclean_grants++;
        return false;
    }
    t->sgated = true;
    state_kick_cores();   // a thread that waits for a core after an unclean grant may have been blocked by this waiter
    gate_block(t, lk, SITE_ACQ);
    t->sgated = false;
    return true;
}

void state_init() {
    g_process_start_ms = GetTickCount64();
    if (const char *c = getenv("SNAP_CHECKPOINT")) std::thread(checkpoint_driver, std::string(c)).detach();
    const char *e = getenv("SNAP_STATE_TEST");
    if (!e) return;
    if (strncmp(e, "census@t", 8) == 0) {
        int start = atoi(e + 8), rounds = 5, gap = 2;
        if (const char *p = strstr(e, ",n")) rounds = atoi(p + 2);
        if (const char *p = strstr(e, ",gap")) gap = atoi(p + 4);
        std::thread(census_driver, start, rounds, gap).detach();
    } else if (strncmp(e, "save@t", 6) == 0) {   // save@t<s>[,load@t<s>][,loops<n>][,lgap<s>]
        TestPlan p;
        p.save_s = atoi(e + 6);
        if (const char *q = strstr(e, ",load@t")) p.load_s = atoi(q + 7);
        if (const char *q = strstr(e, ",loops")) p.loops = atoi(q + 6);
        if (const char *q = strstr(e, ",lgap")) p.lgap = atoi(q + 5);
        p.shots = strstr(e, ",shots") != nullptr;
        std::thread(save_load_driver, p).detach();
    }
}

}  // namespace osk

extern "C" void snap_state_request(int op, const char *name) { osk::state_request(op, name); }
extern "C" int snap_state_idle() { return osk::state_idle() ? 1 : 0; }
extern "C" void snap_state_info(SnapStateInfo *out) { *out = osk::state_info(); }
extern "C" int snap_state_count(const char *kind) {
    const SnapStateInfo i = osk::state_info();
    return kind && kind[0] == 'l' ? i.loads : kind && kind[0] == 's' ? i.saves : i.done;
}
