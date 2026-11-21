// Env-gated performance instrumentation (off unless SNAP_PERF=<output base path> is set; see analysis/perf/).
// Counters are process-wide atomics added to by any thread; frame_mark() (called once per presented frame) turns the deltas since
// the previous mark into one CSV row. A sampler thread writes per-thread CPU time once a second. When SNAP_PERF is unset every
// entry point is a single predictable branch.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace snap_perf {

enum Ctr {
    C_REPLAY = 0,    // SubmitLists: replaying NVN command lists into Vulkan (game thread CPU), includes several items below
    C_QSUBMIT,       // vkQueueSubmit2 calls (draw lists and present)
    C_PACE,          // frame limiter sleep
    C_ACQUIRE,       // vkAcquireNextImageKHR (blocks on the display)
    C_ACQSEM,        // waiting for the timeline value that last used an acquire semaphore
    C_PRESENT,       // vkQueuePresentKHR
    C_WAITSYNC,      // guest nvnSyncWait (CPU waits for the GPU)
    C_FINISH,        // guest nvnQueueFinish
    C_PIPE,          // vkCreateGraphicsPipelines / compute (synchronous compile on the replaying thread)
    C_SHADER,        // program creation (cache read, vkCreateShaderModule, harness)
    C_POOLSYNC,      // SyncPoolRange: guest memory copied into the mirrored buffers
    C_UPLOAD,        // UploadStaticTexture (deswizzle, ASTC/BC7 transcode, staging copy)
    C_HASH,          // RefreshDynamicTexture hashing of CPU-written textures
    C_DESC,          // WriteDescriptors (set allocation + vkUpdateDescriptorSets)
    C_GETPIPE,       // GetPipeline lookup including key building (includes compile)
    C_READBACK,      // FlushReadbacks / GPU->guest copies
    C_SCRATCH,       // NewScratch (vkCreateBuffer + vkAllocateMemory + map)
    C_REAP,          // QueueImpl::Reap
    C_ENSUREIMG,     // TextureImpl::EnsureImage that creates an image (vkCreateImage + memory) (perf pass 4 replay sub-scope)
    C_VIEW,          // image view creation (sampled and attachment views)
    C_RENDBEGIN,     // BeginRendering (render-pass setup incl. attachment views)
    C_BACKING,       // EnsureBacking: pool mirror creation + whole-pool copy (any thread; perf pass 7)
    C_COUNT
};
enum Val {   // plain counters (no time)
    V_DRAWS = 0, V_SUBMITS, V_CMDS, V_POOLSYNC_BYTES, V_POOLSYNC_CALLS, V_UPLOADS, V_UPLOAD_BYTES, V_HASH_BYTES, V_HASH_CALLS,
    V_PIPES_CREATED, V_SCRATCH_ALLOCS, V_DESC_SETS, V_BARRIERS, V_SHADERS, V_PIPES_SKIPPED,
    V_UPLOAD_PREF,   // [perf] static uploads served from the prefetch (deswizzled off the replay thread)
    V_UPLOAD_PREF_IMG,   // [perf] images created by a prefetch worker and adopted by EnsureImage (pass 5)
    V_PREF_MISS_NONE, V_PREF_MISS_FAIL, V_PREF_MISS_HASH, V_PREF_MISS_LATE,   // [perf] static uploads not served by the prefetch: never queued / job failed (bytes changed during it) / bytes changed since / job not started yet
    V_PREF_MISS_DROP,                                                         // [perf] pass 6: queued but dropped from the staging budget before its upload
    V_PREF_SKIP_KIND, V_PREF_SKIP_NOSRC, V_PREF_SKIP_BUDGET,                  // [perf] pass 6: TexPrefetch calls not queued: format/target/size / no guest source yet / budget full
    V_COUNT
};

// SNAP_PERF_SHARED=<prefix> is the same output for a run on one normal (non-exclusive) slot: A/B comparisons on a shared PC.
inline const bool on = std::getenv("SNAP_PERF") != nullptr || std::getenv("SNAP_PERF_SHARED") != nullptr;

// Blocked time by cause (perf pass 3, SNAP_PERF only). Each thread adds to its own ledger (registered on first use; the
// sampler writes <base>_waits.csv once a second, per thread) and to process-wide sums by role: frame_mark writes the deltas of
// the presenting threads (gt_*: the JobWorkers that call nvnQueuePresent), of the main thread (mt_*) and of all threads (all_*).
enum Wt {
    WT_CORE = 0,  // acquire_core: ready, waiting for one of the 3 virtual cores (includes preemption hand-backs)
    WT_LOCK,      // contended nn::os mutex / critical section (svcArbitrateLock path)
    WT_CVAR,      // condition variable wait (events, semaphores, message queues, barriers, cv)
    WT_KEY,       // light events, rw locks, timers, thread join
    WT_MULTI,     // WaitAny / MultiWait
    WT_SLEEP,     // nn::os::SleepThread / svcSleepThread
    WT_FS,        // fs file read (host ReadFile)
    WT_COUNT
};
enum Role : uint8_t { R_OTHER = 0, R_PRESENT = 1, R_MAIN = 2, R_COUNT };
struct Ledger {
    std::atomic<uint64_t> ns[WT_COUNT];
    unsigned long tid = 0;
    bool reg = false;
    ~Ledger();
};
extern thread_local Ledger tl_ledger;
extern thread_local uint8_t tl_role;
extern std::atomic<uint64_t> g_role_ns[R_COUNT][WT_COUNT];
extern std::atomic<uint64_t> g_fs_bytes;
void ledger_register();
unsigned long ledger_tid();
inline void wait_add(Wt w, uint64_t ns) {
    if (!tl_ledger.reg) ledger_register();
    tl_ledger.ns[w].fetch_add(ns, std::memory_order_relaxed);
    g_role_ns[tl_role][w].fetch_add(ns, std::memory_order_relaxed);
}
// perf pass 13: critical-path recorder (SNAP_CRIT=<from>-<to> present numbers, with SNAP_PERF_SHARED / SNAP_PERF). One record per
// blocked interval (who waited, how long, who woke it and when) plus a mark per present; dumped to <base>_crit.csv at the window end.
struct CritRec {
    uint64_t t0, t1, wake_ns;
    uint32_t tid, waker_tid, key, h[3];
    uint8_t type, kind;   // type 0 = wait of Wt kind, 1 = present mark (key = present number)
};
extern bool g_crit_on;
extern std::atomic<bool> g_crit_active;
void crit_push(const CritRec& r);
inline void crit_wait(Wt w, uint64_t t1, uint64_t ns, uint32_t waker_tid, uint64_t wake_ns, uint64_t key, const uint32_t* h) {
    if (!g_crit_active.load(std::memory_order_relaxed)) return;
    CritRec r{};
    r.t0 = t1 - ns; r.t1 = t1; r.wake_ns = wake_ns; r.tid = ledger_tid(); r.waker_tid = waker_tid; r.key = (uint32_t)key;
    if (h) { r.h[0] = h[0]; r.h[1] = h[1]; r.h[2] = h[2]; }
    r.type = 0; r.kind = (uint8_t)w;
    crit_push(r);
}
inline void set_role(Role r) { tl_role = r; }
inline bool key_thread() { return tl_role != R_OTHER; }
// Long-wait event threshold in ns (SNAP_PERF_WAIT_MS, default 8 ms) for presenting / main threads; other threads' core waits use 4x.
extern uint64_t g_wait_event_ns;

inline uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern std::atomic<uint64_t> g_ns[C_COUNT], g_calls[C_COUNT], g_val[V_COUNT];
// Skinning dispatch barrier elision (nvn_draw.cpp DoDispatch, perf pass 18): dispatch barriers dropped, and dropped barriers the overlap guard put
// back. The frames csv ends with these two as per-frame deltas (cs_elided, cs_forced); cs_forced above 0 means the guard fired.
inline std::atomic<uint64_t> g_cs_elided{0}, g_cs_forced{0};

inline void add(Ctr c, uint64_t ns) {
    g_ns[c].fetch_add(ns, std::memory_order_relaxed);
    g_calls[c].fetch_add(1, std::memory_order_relaxed);
    if (ns >= 30000 && g_crit_active.load(std::memory_order_relaxed)) {   // SNAP_CRIT: a timed scope (30 us or longer) on this thread (type 2, kind = counter)
        CritRec r{};
        const uint64_t t1 = now_ns();
        r.t0 = t1 - ns; r.t1 = t1; r.tid = ledger_tid(); r.type = 2; r.kind = (uint8_t)c;
        crit_push(r);
    }
}
inline void count(Val v, uint64_t n = 1) { if (on) g_val[v].fetch_add(n, std::memory_order_relaxed); }

struct Scope {
    Ctr c; uint64_t t0;
    explicit Scope(Ctr c_) : c(c_), t0(on ? now_ns() : 0) {}
    ~Scope() { if (on) add(c, now_ns() - t0); }
};

// One row per presented frame. gpu_* come from the NVN layer's timestamp queries (-1 when unavailable).
void frame_mark(uint64_t present_no, double gpu_busy_ms, double gpu_span_ms, int gpu_cbs);
// A named occurrence with a duration (pipeline compiles, shader translations, texture uploads over 2 ms ...).
void event(const char* kind, double ms, const char* detail);
// Drains the event ring to the file and flushes it (atexit, exit and crash paths). crash: try-locks only, never blocks.
void flush_events(bool crash);

}  // namespace snap_perf

extern "C" void snap_perf_flush(int crash);   // the same, for callers that do not include this header's C++ API
