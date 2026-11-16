// Skip course intro (project code, ours). Off unless SNAP_SKIP_INTRO=1 (settings key skip_course_intro).
// The course rail (R) is held still by R+0x1c = +inf from spawn until the course state machine sends message 0x11e4ec2f (which stores 0), after
// a fixed-length intro (NEO-ONE warp-in, orbiting camera, about 10 s of game time). With the option on, every rail update during that hold moves
// guest time forward (os_time_skew), so the game's own timers, effects and camera run the intro in a fraction of the real time and finish it
// the normal way (same camera hand-off, same release message). Nothing is edited in the game's state.
// Dev switches: SNAP_INTRO_TRACE=1 logs the course rail's timer fields once per 15 presents; SNAP_INTRO_WATCH=1 puts a hardware write
// watchpoint on R+0x1c of the first course rail and logs the rip and return addresses of every distinct writer (resolve with the linker map).
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

extern "C" uint64_t snap_present_count();   // host/boot.cpp
extern "C" void os_time_skew(double seconds);   // runtime/os/os_kernel.cpp

namespace {
bool g_inited, g_trace, g_watch, g_skip;
double g_k = 8.0, g_last, g_skewed;   // clock speed-up during the intro; QPC seconds of the previous held rail update; total skipped
uint64_t g_frames;
constexpr double kMaxStep = 0.2;   // longest game-time step per frame (the game's clock falls back to 1/60 s above 0.5 s)
std::atomic<uint64_t> g_watchAddr{0};
uint64_t g_modbase;

LONG CALLBACK veh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if (!(ep->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    static std::atomic<int> n{0};
    static std::atomic<uint64_t> seen[64];
    const uint64_t rip = ep->ContextRecord->Rip;
    bool fresh = true;
    for (int i = 0; i < 64; i++) if (seen[i].load() == rip) { fresh = false; break; }
    if (fresh) {
        int k = n.fetch_add(1);
        if (k < 64) {
            seen[k] = rip;
            void* fr[10] = {};
            int got = RtlCaptureStackBackTrace(0, 10, fr, nullptr);
            const uint64_t a = g_watchAddr.load();
            fprintf(stderr, "[intro-watch] present=%llu rip=exe+%#llx val=%g bt:", (unsigned long long)snap_present_count(), (unsigned long long)(rip - g_modbase),
                    IsBadReadPtr((void*)a, 4) ? 0.0f : *(float*)a);
            for (int i = 0; i < got; i++) fprintf(stderr, " exe+%#llx", (unsigned long long)((uint64_t)fr[i] - g_modbase));
            fprintf(stderr, "\n");
        }
    }
    ep->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void arm_all() {
    const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
        if (!h) continue;
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT cx{};
            cx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(h, &cx)) {
                cx.Dr0 = g_watchAddr.load();
                cx.Dr7 |= 1ull | (1ull << 16) | (3ull << 18);   // enable, write, 4 bytes
                SetThreadContext(h, &cx);
            }
            ResumeThread(h);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
}

void watch_thread() {
    g_modbase = (uint64_t)GetModuleHandleA(nullptr);
    AddVectoredExceptionHandler(1, veh);
    for (int i = 0; i < 40; i++) { arm_all(); Sleep(1000); }
}

void init() {
    g_inited = true;
    const char* t = getenv("SNAP_INTRO_TRACE");
    g_trace = t && *t && *t != '0';
    const char* w = getenv("SNAP_INTRO_WATCH");
    g_watch = w && *w && *w != '0';
    const char* k = getenv("SNAP_SKIP_INTRO");
    g_skip = k && *k && *k != '0';
    if (const char* x = getenv("SNAP_SKIP_INTRO_X")) { double v = atof(x); if (v >= 2.0 && v <= 30.0) g_k = v; }   // dev: speed-up factor
}
}  // namespace

// Called from rt_gm_neo_speed (host/mods/neofilm.cpp, MAIN_INLINE_HOOKS main+0x4c1df4): R = the rail object, once per rail update.
extern "C" void snap_intro_rail(uint64_t R) {
    if (!g_inited) init();
    if ((!g_trace && !g_watch && !g_skip) || !R || IsBadReadPtr((void*)R, 0x200)) return;
    uint64_t curve = *(uint64_t*)(R + 0x30);
    if (!curve || IsBadReadPtr((void*)curve, 0x40)) return;
    float len = *(float*)(curve + 0x3c);
    if (len < 100.0f) return;
    if (g_skip && *(uint32_t*)(R + 0x1c) == 0x7f800000u && *(float*)(R + 0x70) == 0.0f) {
        // the rail is held (intro running): run the game's clock g_k times faster than real time, at most kMaxStep per frame
        LARGE_INTEGER n, f;
        QueryPerformanceCounter(&n);
        QueryPerformanceFrequency(&f);
        const double now = (double)n.QuadPart / (double)f.QuadPart;
        const double real = g_last > 0.0 ? now - g_last : 0.0;
        g_last = now;
        if (real > 0.0 && real < 0.25) {
            const double add = std::min((g_k - 1.0) * real, std::max(0.0, kMaxStep - real));
            os_time_skew(add);
            g_skewed += add;
            g_frames++;
        }
    } else {
        g_last = 0.0;
        if (g_frames) {
            fprintf(stderr, "[intro] present=%llu intro done: %llu frames, %.2f s of game time skipped\n", (unsigned long long)snap_present_count(),
                    (unsigned long long)g_frames, g_skewed);
            g_frames = 0;
        }
    }
    if (g_watch && !g_watchAddr.load()) {
        g_watchAddr = R + 0x1c;
        std::thread(watch_thread).detach();
    }
    if (!g_trace) return;
    static uint64_t last;
    uint64_t fr = snap_present_count();
    if (fr < last + 15) return;
    last = fr;
    fprintf(stderr, "[intro] present=%llu R=%#llx len=%.2f prog=%.4f +1c=%.4f +20=%.4f +24=%.4f +28=%.4f +14=%.4f +1db=%d +154=%d +15c=%d\n",
            (unsigned long long)fr, (unsigned long long)R, len, *(float*)(R + 0x70), *(float*)(R + 0x1c), *(float*)(R + 0x20), *(float*)(R + 0x24),
            *(float*)(R + 0x28), *(float*)(R + 0x14), *(unsigned char*)(R + 0x1db), *(unsigned char*)(R + 0x154), *(unsigned char*)(R + 0x15c));
}
