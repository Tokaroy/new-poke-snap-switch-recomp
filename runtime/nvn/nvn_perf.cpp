// SPDX-License-Identifier: MIT
// GPU-side half of the SNAP_PERF instrumentation (runtime/os/perf.h): a ring of timestamp pairs around every command buffer the
// layer submits. Slots are collected at the next present once the queue's timeline has passed them. Only active with SNAP_PERF set.
// All entry points except PerfFrameMark are called with Host::queueMutex held (SubmitLists / PresentWindow); it takes the lock itself.
// The performance overlay's Detailed mode (runtime/ui/ui_fps.cpp) turns the same GPU timing on at run time (snap_perf_overlay_timing)
// and reads GPU busy ms and the presenting thread's CPU ms per frame (snap_perf_overlay_info); no CSV is written then.
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

#include "nvn_aa.h"
#include "nvn_internal.h"
#include "os/perf.h"
#include "upscale/upscaler.h"

namespace nvn {

std::atomic<bool> g_overlayTiming{false};
bool PerfGpuOn() { return snap_perf::on || g_overlayTiming.load(std::memory_order_relaxed); }
namespace {
std::atomic<double> g_ovGpuMs{-1.0}, g_ovCpuMs{-1.0};
}  // namespace

namespace {
constexpr uint32_t kSlots = 2048;
struct Slot { uint64_t value = 0; bool submitted = false; bool used = false; };
VkQueryPool g_pool = VK_NULL_HANDLE;
Slot g_slots[kSlots];
uint32_t g_next = 0;
bool g_failed = false;
// SNAP_GPU_PASSES: marks per command buffer slot
struct Mark { uint32_t q; uint32_t draws; char label[96]; };
std::vector<Mark> g_marks[kSlots];
uint32_t g_startDraws[kSlots], g_endDraws[kSlots];
int g_curSlot = -1;
VkCommandBuffer g_curCb = VK_NULL_HANDLE;
VkQueryPool g_mpool = VK_NULL_HANDLE;
constexpr uint32_t kMarkQ = 32768;
uint32_t g_markNext = 0;
FILE* g_pf = nullptr;
}  // namespace

bool g_perfPassOn = false;
uint32_t g_perfDraws = 0;

void PerfMark(Host* h, VkCommandBuffer cb, const char* label) {
  if (!g_perfPassOn || g_curSlot < 0 || cb != g_curCb) return;
  if (!g_mpool) {
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kMarkQ;
    if (vkCreateQueryPool(h->device, &qi, nullptr, &g_mpool) != VK_SUCCESS) { g_perfPassOn = false; return; }
  }
  const uint32_t q = g_markNext++ % kMarkQ;
  vkResetQueryPool(h->device, g_mpool, q, 1);
  vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g_mpool, q);
  Mark m{q, g_perfDraws, {}};
  snprintf(m.label, sizeof m.label, "%s", label);
  g_marks[g_curSlot].push_back(m);
}

bool PerfGpuReady(Host* h) {
  if (!PerfGpuOn() || g_failed) return false;
  if (g_pool) return true;
  if (const char* pp = getenv("SNAP_GPU_PASSES"); pp && *pp && (g_pf = fopen(pp, "w"))) {
    fprintf(g_pf, "present,slot,idx,ms,draws,label\n");
    g_perfPassOn = true;
  }
  if (!h->hostQueryReset) { g_failed = true; return false; }
  VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kSlots * 2;
  if (vkCreateQueryPool(h->device, &qi, nullptr, &g_pool) != VK_SUCCESS) { g_failed = true; g_pool = VK_NULL_HANDLE; return false; }
  return true;
}

int PerfGpuBegin(Host* h, VkCommandBuffer cb) {
  if (!PerfGpuReady(h)) return -1;
  const uint32_t s = g_next++ % kSlots;
  if (g_slots[s].used) return -1;   // ring full of uncollected slots: skip this one
  g_slots[s] = {0, false, true};
  vkResetQueryPool(h->device, g_pool, s * 2, 2);
  vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g_pool, s * 2);
  g_marks[s].clear(); g_startDraws[s] = g_perfDraws; g_curSlot = (int)s; g_curCb = cb;
  return (int)s;
}

void PerfGpuEnd(Host*, VkCommandBuffer cb, int slot) {
  if (slot < 0) return;
  vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g_pool, (uint32_t)slot * 2 + 1);
  g_endDraws[slot] = g_perfDraws; g_curSlot = -1; g_curCb = VK_NULL_HANDLE;
}

void PerfGpuSubmitted(int slot, uint64_t value) {
  if (slot < 0) return;
  g_slots[slot].value = value; g_slots[slot].submitted = true;
}

void PerfFrameMark(QueueImpl* q, uint64_t presentNo) {
  if (!PerfGpuOn()) return;
  std::lock_guard<std::mutex> lk(q->host->queueMutex);
  double busy = -1, span = -1; int cbs = 0;
  if (g_pool) {
    uint64_t done = 0;
    vkGetSemaphoreCounterValue(q->host->device, q->timeline, &done);
    const double period = q->host->props.limits.timestampPeriod;
    uint64_t lo = UINT64_MAX, hi = 0, sum = 0;
    for (uint32_t s = 0; s < kSlots; s++) {
      Slot& sl = g_slots[s];
      if (!sl.used || !sl.submitted || sl.value > done) continue;
      uint64_t t[2] = {0, 0};
      if (vkGetQueryPoolResults(q->host->device, g_pool, s * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] >= t[0]) {
        sum += t[1] - t[0]; lo = std::min(lo, t[0]); hi = std::max(hi, t[1]); cbs++;
        if (g_perfPassOn && g_pf && !g_marks[s].empty()) {   // per-pass rows: each segment runs from its mark to the next one (or the end)
          const auto& ms = g_marks[s];
          const double per = q->host->props.limits.timestampPeriod / 1e6;
          uint64_t prev = t[0]; uint32_t prevDraws = g_startDraws[s]; const char* prevLabel = "cb-start"; int idx = 0;
          for (size_t k = 0; k <= ms.size(); k++) {
            uint64_t at = t[1]; uint32_t dr = g_endDraws[s];
            if (k < ms.size()) {
              uint64_t mt = 0;
              if (vkGetQueryPoolResults(q->host->device, g_mpool, ms[k].q, 1, 8, &mt, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) mt = prev;
              at = mt; dr = ms[k].draws;
            }
            if (at < prev) at = prev;
            fprintf(g_pf, "%llu,%u,%d,%.4f,%u,%s\n", (unsigned long long)presentNo, s, idx++, (double)(at - prev) * per, dr - prevDraws, prevLabel);
            prev = at; prevDraws = dr;
            if (k < ms.size()) prevLabel = ms[k].label;
          }
        }
      }
      sl.used = false; sl.submitted = false;
    }
    busy = (double)sum * period / 1e6;
    span = hi > lo ? (double)(hi - lo) * period / 1e6 : 0;
  }
  if (g_overlayTiming.load(std::memory_order_relaxed)) {
    // CPU: the presenting thread's cycle count per frame (the SNAP_PERF cpu_mcycles source), in ms via the TSC rate measured
    // against QueryPerformanceCounter (QueryThreadCycleTime counts TSC ticks)
    // presents can come from different guest worker threads: a delta counts only when the previous present was on this thread too
    static ULONG64 prevCyc = 0; static DWORD prevTid = 0;
    const DWORD tid = GetCurrentThreadId();
    if (tid != prevTid) { prevTid = tid; prevCyc = 0; }
    static uint64_t tsc0 = 0; static LARGE_INTEGER qpc0{}, qpf{};
    static double tscPerMs = 0;
    ULONG64 cyc = 0; QueryThreadCycleTime(GetCurrentThread(), &cyc);
    const uint64_t tsc = __rdtsc(); LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
    if (!qpf.QuadPart) { QueryPerformanceFrequency(&qpf); tsc0 = tsc; qpc0 = qpc; }
    const double ms = (double)(qpc.QuadPart - qpc0.QuadPart) * 1000.0 / (double)qpf.QuadPart;
    if (ms > 200.0) { tscPerMs = (double)(tsc - tsc0) / ms; if (ms > 5000.0) { tsc0 = tsc; qpc0 = qpc; } }
    if (prevCyc && tscPerMs > 0 && cyc >= prevCyc) g_ovCpuMs.store((double)(cyc - prevCyc) / tscPerMs);
    prevCyc = cyc;
    if (busy >= 0 && cbs > 0) g_ovGpuMs.store(busy);
  }
  if (g_pf) fflush(g_pf);
  if (snap_perf::on) snap_perf::frame_mark(presentNo, busy, span, cbs);
}

}  // namespace nvn

extern "C" void snap_perf_overlay_timing(int on) {
  nvn::g_overlayTiming.store(on != 0);
  if (!on) { nvn::g_ovGpuMs.store(-1.0); nvn::g_ovCpuMs.store(-1.0); }
}

// Values for the Detailed overlay: GPU busy and CPU ms of the last collected frame (-1 = none yet), the 3D scene size, the game's
// pace rate and a one-line upscaler / anti-aliasing description (aa may be null or aa_len 0).
extern "C" void snap_perf_overlay_info(double* gpu_ms, double* cpu_ms, int* render_w, int* render_h, double* pace_hz, char* aa, int aa_len) {
  using namespace nvn;
  if (gpu_ms) *gpu_ms = g_ovGpuMs.load();
  if (cpu_ms) *cpu_ms = g_ovCpuMs.load();
  if (render_w) *render_w = (int)(1920.0f * g_scale.scale + 0.5f);
  if (render_h) *render_h = (int)(1080.0f * g_scale.scale + 0.5f);
  if (pace_hz) *pace_hz = g_paceHz;
  if (aa && aa_len > 0) {
    const char* up = snap_upscale::Name();
    const char* m = g_aaMode == AA_SMAA ? "SMAA" : g_aaMode == AA_OFF ? "AA off" : "FXAA (original)";
    char sh[24] = "";
    if (g_aaSharpen > 0.f) snprintf(sh, sizeof sh, "  CAS %.2f", g_aaSharpen);
    if (up && strcmp(up, "off")) snprintf(aa, (size_t)aa_len, "%s  %s%s", up, m, sh);
    else snprintf(aa, (size_t)aa_len, "%s%s", m, sh);
  }
}
