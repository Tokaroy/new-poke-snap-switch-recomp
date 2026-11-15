// SPDX-License-Identifier: MIT
// Refined model packs, renderer side (docs/hd-models.md). A refined mesh object (scripts/hd/mesh_refine) carries two
// triangle sets in one index range:
//
//   H (9) | original triangles | S (9) | refined triangles | T (9)
//
// H, S and T are degenerate triangles (H = 0,0,0 1,1,1 0,0,0, S = 1,1,1 0,0,0 1,1,1, T = 0,0,0 0,0,0 1,1,1). Its
// vertices 0..V-1 are the original vertices, byte for byte, with their original skin weights; the refined triangles add
// vertices after them. Here each indexed draw of such an object is narrowed to one of the two sets:
//   - the photo evaluation pass (its depth target is tagged at creation, nvn_entry.cpp) always gets the original set, so
//     the subject and occluder coverage that photo scoring reads is the original game's;
//   - every other pass gets the refined set, unless SNAP_MODEL_REFINE=0 (original everywhere) or the SNAP_MODS_AB /
//     SNAP_MODEL_AB test aid shows the original on odd (present / P) periods (same convention as the texture A/B).
// Draws that are not refined objects are untouched (one 18-byte compare of their first indices).
//   SNAP_MODEL_REFINE=0|1   1 (default) = draw the refined set where a pack provides one; 0 = original set everywhere
//   SNAP_MODEL_AB=<P>       A/B period (falls back to SNAP_MODS_AB); odd periods draw the original set
//   SNAP_MODEL_LOG=1        log the first narrowed draws and a per-600-present count
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "nvn_internal.h"
#include "nvn_state.h"

extern "C" unsigned snap_mods_ab_period();
extern "C" void snap_perf_overlay_timing(int on);   // nvn_perf.cpp: GPU busy time per frame without SNAP_PERF
extern "C" void snap_perf_overlay_info(double* gpu_ms, double* cpu_ms, int* render_w, int* render_h, double* pace_hz, char* aa, int aa_len);

namespace nvn {
namespace {
std::mutex g_mu;
std::unordered_set<const TextureImpl*> g_evalDepth;              // photo evaluation depth targets (tagged at creation)
struct Split { int count; int sep; };
std::unordered_map<uint64_t, Split> g_split;                      // index address -> separator position
const uint16_t kH[9] = {0, 0, 0, 1, 1, 1, 0, 0, 0};
const uint16_t kS[9] = {1, 1, 1, 0, 0, 0, 1, 1, 1};
const uint16_t kT[9] = {0, 0, 0, 0, 0, 0, 1, 1, 1};

int Mode() {
  static const int m = getenv("SNAP_MODEL_REFINE") ? atoi(getenv("SNAP_MODEL_REFINE")) : 1;
  return m;
}
unsigned AbPeriod() {
  static const unsigned p = getenv("SNAP_MODEL_AB") ? (unsigned)strtoul(getenv("SNAP_MODEL_AB"), nullptr, 0) : snap_mods_ab_period();
  return p;
}
bool LogOn() {
  static const bool on = getenv("SNAP_MODEL_LOG") != nullptr;
  return on;
}
// SNAP_MODEL_LOG=2: also every narrowed draw at the presents listed in NVN_FRAME_DUMP_AT (ties a frame dump to the meshes in it).
bool DumpPresent(int p) {
  static const std::unordered_set<int> at = [] {
    std::unordered_set<int> s;
    const char* l = getenv("SNAP_MODEL_LOG");
    const char* e = getenv("NVN_FRAME_DUMP_AT");
    if (l && atoi(l) >= 2 && e)
      for (const char* q = e; *q;) { char* end; long v = strtol(q, &end, 10); if (end == q) { q++; continue; } s.insert((int)v); q = end; }
    return s;
  }();
  return !at.empty() && at.count(p) != 0;
}
// SNAP_MODEL_GPU=<file>: one line per present, "present gpu_ms refined_draws original_draws". gpu_ms is the last frame the
// GPU timer collected, which lags the present by a frame or two; with the A/B on, scripts/hd/mesh_refine/gpu_cost.py pairs
// the two by the lag that separates them best. Turns on the overlay's GPU timer (timestamps around each command buffer).
int g_curRef = 0, g_curOrig = 0;
void GpuSample() {
  static const char* path = getenv("SNAP_MODEL_GPU");
  if (!path) return;
  static FILE* f = nullptr;
  static int last = -1;
  if (!f) { f = fopen(path, "w"); if (!f) { path = nullptr; return; } snap_perf_overlay_timing(1); }
  const int p = g_presentCount;
  if (p == last) return;
  if (last >= 0) {
    double gpu = -1;
    snap_perf_overlay_info(&gpu, nullptr, nullptr, nullptr, nullptr, nullptr, 0);
    fprintf(f, "%d %.4f %d %d\n", last, gpu, g_curRef, g_curOrig);
    if ((last & 255) == 0) fflush(f);
  }
  last = p; g_curRef = g_curOrig = 0;
}
}  // namespace

// nvn_entry.cpp TextureInitialize: the texture was created by the game's "Evaluation DepthStencil" call site.
void ModelNoteEvalDepth(const TextureImpl* t) {
  std::lock_guard<std::mutex> g(g_mu);
  g_evalDepth.insert(t);
  if (LogOn()) fprintf(stderr, "[model] evaluation depth target %p (%dx%d)\n", (const void*)t, t->b.width, t->b.height);
}
// Entries are never removed: replay can still run evaluation draws after the guest finalizes the target, and a stale entry
// whose address is reused only makes that target draw original meshes (the safe direction for scoring).

// DoDraw (nvn_draw.cpp): returns true and fills `adj` when `c` draws a refined object; `adj` then draws one of its sets.
bool ModelDrawAdjust(ReplayCtx& cx, const Cmd& c, Cmd& adj) {
  GpuSample();
  if (c.op != Op::DrawIndexed || c.i[1] != IDX_U16 || c.i[2] < 27) return false;
  PoolImpl* pool = nullptr; uint64_t off = 0;
  if (!cx.h->ResolveAddress(c.a[0], &pool, &off) || !pool || !pool->storage) return false;
  const uint16_t* ix = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(pool->storage) + off);
  if (memcmp(ix, kH, sizeof kH) != 0) return false;
  const int n = c.i[2];
  if (memcmp(ix + n - 9, kT, sizeof kT) != 0) {
    static int warned = 0;
    if (warned++ < 4) fprintf(stderr, "[model] draw of %d indices starts like a refined object but has no trailer (partial draw?); drawn unchanged\n", n);
    return false;
  }
  int sep = -1;
  bool eval;
  {
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_split.find(c.a[0]);
    if (it != g_split.end() && it->second.count == n && memcmp(ix + it->second.sep, kS, sizeof kS) == 0) sep = it->second.sep;
    eval = cx.rt[8] && g_evalDepth.count(cx.rt[8]) != 0;
  }
  if (sep < 0) {
    for (int k = 9; k + 9 <= n - 9; k += 3)
      if (ix[k] == ix[k + 1] && ix[k + 1] == ix[k + 2]) {   // the first degenerate triangle after H must be S
        if (memcmp(ix + k, kS, sizeof kS) == 0) sep = k;
        break;
      }
    if (sep < 0) {
      static int warned = 0;
      if (warned++ < 4) fprintf(stderr, "[model] refined object at 0x%llx (%d indices) has no separator; drawn unchanged\n", (unsigned long long)c.a[0], n);
      return false;
    }
    std::lock_guard<std::mutex> g(g_mu);
    g_split[c.a[0]] = Split{n, sep};
  }
  const unsigned P = AbPeriod();
  const bool orig = eval || Mode() == 0 || (P && ((unsigned)g_presentCount / P) % 2 == 1);
  if (!eval) (orig ? g_curOrig : g_curRef)++;
  adj = c;
  if (orig) { adj.a[0] = c.a[0] + 9 * 2; adj.i[2] = sep - 9; }
  else { adj.a[0] = c.a[0] + (uint64_t)(sep + 9) * 2; adj.i[2] = n - 9 - (sep + 9); }
  if (LogOn()) {
    static int logged = 0, perOrig = 0, perRef = 0, perEval = 0, lastBucket = -1;
    (orig ? perOrig : perRef)++;
    if (eval) perEval++;
    if (DumpPresent(g_presentCount)) fprintf(stderr, "[model] dump p%d draw %d idx -> %s set (%d indices)%s\n", g_presentCount, n, orig ? "original" : "refined", adj.i[2], eval ? " [evaluation pass]" : "");
    else if (logged < 40) { logged++; fprintf(stderr, "[model] p%d draw %d idx at 0x%llx -> %s set (%d indices)%s\n", g_presentCount, n, (unsigned long long)c.a[0], orig ? "original" : "refined", adj.i[2], eval ? " [evaluation pass]" : ""); }
    if (g_presentCount / 600 != lastBucket) {
      if (lastBucket >= 0) fprintf(stderr, "[model] presents %d..: refined draws %d, original draws %d (evaluation %d)\n", lastBucket * 600, perRef, perOrig, perEval);
      lastBucket = g_presentCount / 600; perOrig = perRef = perEval = 0;
    }
  }
  return true;
}

}  // namespace nvn
