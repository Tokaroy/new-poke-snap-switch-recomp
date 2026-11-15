// SPDX-License-Identifier: MIT
// Interface pacing and pad sampling above 60 fps (UI-HZ-01, INPUT-HZ-01; analysis/perf/high-fps.md).
//
// The game's interface runs once per game frame and counts frames, so with SNAP_FPS=120|144 it ran 2 to 2.4 times too fast (menus,
// dialogue, hint panels) while the 3D scene, which uses the measured timer, kept its speed. Three inline hooks in main (recomp/snap_gen.py
// UI_PACE_INLINE; they exist only in a main generated with SNAP_GEN_UI_PACE=1) fix that. Each returns at once when the effective pace is
// 60 fps or below (SNAP_FPS unset or 60, or a frame limit under 60), so the default game is unchanged.
//
//   rt_gm_ui_enter    handler main+0x7835d0 after it saved its object (x0). x16 = 1 run the handler, 0 branch to its drawing section
//                     (0x794b4c): the object's logic, timeline advance and event 22 are skipped, the movies are still drawn. Only an
//                     object the handler would carry on with (started +0xc0, ready +0xc6, not held +0xc4) is skipped.
//   rt_gm_ui_event    before the handler raises the per-frame event 22. x16 = 0 leaves through the handler's exit (0x795f08).
//   rt_gm_ui_pad      the pad update's epilogue (main+0x925b60 ... 0x9263ac), x19 = pad. Marks the game frame (one decision per game
//                     frame, so pad and handlers agree) and, above 60 fps, makes the pad change only on interface steps: other frames
//                     show the last step's buttons and no new edges; a step compares with the last step; a press that came and went
//                     in between still counts. Sticks are not touched.
//
//   SNAP_UI_PACE=0     kill switch for the handler and event hooks (every frame steps, as without this file).
//   SNAP_PAD_PACE=0    kill switch for the pad sampling (the pad stays as the game wrote it).
//   SNAP_UI_PACE_TRACE=1  logs the handler counts every 5 s ("[uipace]" in stderr).
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "loader.h"
#include "uipace_core.h"

extern "C" uint64_t snap_present_count();   // host/boot.cpp
extern "C" int snap_window_frame_cap();     // host/window_opts.cpp: fps, 0 = none (nvn Pace() reads it too)
namespace nvn { extern double g_paceHz; }   // runtime/nvn/nvn_vk.cpp: rate of present interval 1, 0 = display-paced only

namespace {

bool g_ui_on = true, g_pad_on = true, g_trace;
std::atomic<bool> g_cfg{false};

uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void cfg_once() {
    if (g_cfg.load(std::memory_order_acquire)) return;
    if (const char* e = std::getenv("SNAP_UI_PACE")) g_ui_on = !(*e == '0' && !e[1]);
    if (const char* e = std::getenv("SNAP_PAD_PACE")) g_pad_on = !(*e == '0' && !e[1]);
    if (const char* e = std::getenv("SNAP_UI_PACE_TRACE")) g_trace = *e && *e != '0';
    if (g_trace || !g_ui_on || !g_pad_on) std::fprintf(stderr, "[uipace] ui=%d pad=%d trace=%d\n", (int)g_ui_on, (int)g_pad_on, (int)g_trace);
    g_cfg.store(true, std::memory_order_release);
}

// The pace period the presents follow (nvn_vk.cpp Pace(): the target's period, never shorter than the frame limit's). 0 = unpaced.
uint64_t period_ns() {
    const double hz = nvn::g_paceHz;
    uint64_t p = hz > 0 ? (uint64_t)(1e9 / hz) : 0;
    if (const int cap = snap_window_frame_cap(); cap > 0 && p) {
        const uint64_t c = (uint64_t)(1e9 / cap);
        if (c > p) p = c;
    }
    return p;
}

// The game frame: the pad update runs once in each, so the decision is made there and the handlers until its next update use it.
// With no pad update for PAD_GAP_NS the decision falls back to once per presented frame.
constexpr uint64_t kPadGapNs = 100000000ull;
uipace::Pace g_pace, g_pace_presented;
std::atomic<uint64_t> g_pad_tick_ns{0}, g_pad_dt_ns{0};
uint64_t g_pad_tick;
uipace::PadState g_pads[4];
uint64_t g_pad_ptr[4];

int pace_step(uint64_t now) {
    const uint64_t seen = g_pad_tick_ns.load(std::memory_order_acquire);
    if (seen && now >= seen && now - seen < kPadGapNs) return g_pace.step;
    return uipace::decide(g_pace_presented, snap_present_count(), now, period_ns());
}

uint64_t g_ran, g_drawn, g_win_ns, g_win_present, g_edges, g_steps, g_pad_frames;

void trace_counts(uint64_t now, int ran) {
    if (ran) g_ran++; else g_drawn++;
    const uint64_t fr = snap_present_count();
    if (!g_win_ns) g_win_ns = now, g_win_present = fr;
    if (now - g_win_ns < 5000000000ull) return;
    const double s = (double)(now - g_win_ns) / 1e9;
    std::fprintf(stderr, "[uipace] %.1f s: %.1f presents/s, handlers run %.1f/s, draw only %.1f/s, period %.3f ms; game frames %.1f/s, steps %.1f/s, pad press edges %llu\n", s,
                 (double)(snap_present_count() - g_win_present) / s, (double)g_ran / s, (double)g_drawn / s, (double)period_ns() / 1e6, (double)g_pad_frames / s,
                 (double)g_steps / s, (unsigned long long)g_edges);
    g_win_ns = now, g_win_present = fr, g_ran = g_drawn = g_pad_frames = g_steps = 0;
}

}  // namespace

extern "C" void rt_gm_ui_enter(Cpu* c) {
    c->x[16] = 1;
    cfg_once();
    if (!g_ui_on) return;
    const uint64_t now = now_ns();
    int step = pace_step(now);
    if (!step) {
        // Only an object the handler would carry on with may be drawn without a step: started (+0xc0), ready (+0xc6), not held (+0xc4).
        const uint8_t* o = (const uint8_t*)c->x[0];
        if (!o || !o[0xc0] || !o[0xc6] || o[0xc4]) step = 1;
    }
    c->x[16] = (uint64_t)step;
    if (g_trace) trace_counts(now, step);
}

extern "C" void rt_gm_ui_event(Cpu* c) {
    c->x[16] = 1;
    cfg_once();
    if (!g_ui_on) return;
    c->x[16] = (uint64_t)pace_step(now_ns());
}

extern "C" void rt_gm_ui_pad(Cpu* c) {
    cfg_once();
    const uint64_t pad = c->x[19];
    if (!pad || (!g_ui_on && !g_pad_on)) return;
    const uint64_t now = now_ns(), period = period_ns();
    int slot = 0;
    while (slot < 4 && g_pad_ptr[slot] && g_pad_ptr[slot] != pad) slot++;
    if (slot == 4) return;   // more pads than the game has: leave them as they are
    g_pad_ptr[slot] = pad;
    if (slot == 0) {   // the first pad marks the game frame
        const uint64_t before = g_pad_tick_ns.load(std::memory_order_relaxed);
        g_pad_dt_ns.store(before && now > before ? now - before : 0, std::memory_order_relaxed);
        uipace::decide(g_pace, g_pad_tick++, now, g_ui_on ? period : 0);
        g_pad_tick_ns.store(now, std::memory_order_release);
    }
    if (!g_pad_on) return;
    uint32_t* mask = (uint32_t*)(pad + uipace::kPadMaskOff);
    uint8_t* bytes = (uint8_t*)(pad + uipace::kPadBytesOff);
    uipace::sample_pad(g_pads[slot], mask, bytes, g_pace.step != 0, !g_ui_on || !uipace::above60(period));
    if (g_trace && slot == 0) {   // the pad as the game will read it: every press edge it sees (compare 60 and 144 with the same script)
        g_pad_frames++;
        g_steps += g_pace.step != 0;
        g_edges += (uint64_t)__builtin_popcount(mask[1]);
    }
}
