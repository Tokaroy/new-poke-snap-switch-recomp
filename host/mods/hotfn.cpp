// Hot leaf routines of main on the host (decomp-for-speed pass, 2026-11-20; project code, ours). The native versions are in
// hotfn.h; tests/hotfn/run_hotfn_test.py compares them with the generated guest functions (bit-exact, random and real inputs).
//
// Each is an inline hook at the function entry plus a branch hook to its `ret` (recomp/snap_gen.py HOTFN_INLINE /
// HOTFN_BRANCH, emitted when the generator runs with SNAP_GEN_HOTFN=1): the hook does the whole function, leaves c->x[16] = 0
// and the generated code jumps to the return. It writes the same memory as the guest; the caller-saved scratch registers
// (x8-x17, v1-v7, v16-v31) are left as they were, which AAPCS allows (every call site was checked: none reads them after
// the call before writing them).
//
//   main+0x96d390  MatrixDecompose         scale / quaternion / translation of a matrix (JobWorker, per bone or node)
//                                          SNAP_HOT_MATDECOMP
//   main+0x96d5f0  ScaledMatrixDecompose   the same after a per-axis scale (JobWorker)            SNAP_HOT_MATDECOMP_SCALED
//   main+0x155600  ReverbFdn4              Wwise reverb 4-line delay network, one channel (audio) SNAP_HOT_REVERB
//   main+0x8b5f10  HierarchyTransform      recursive child-node world matrices (inverse(parent) x child)  SNAP_HOT_HIER
//   main+0x8b6750  bone update, the two per-bone loop bodies (local matrix + decompose): mid-function hooks at
//                  0x8b7400 / 0x8b7ac0 that branch to the instruction after the body's last bl     SNAP_HOT_BONE
//
// Each switch: unset or 1 = native, 0 = the guest code, check = the guest runs and the native result (computed first on
// copies) is compared with it just before the guest's final store (rt_hot_*_check); the first mismatches and a count at
// every 4096th call go to stderr. SNAP_HOTFN_CAPTURE=<file> appends the inputs of the first calls of each function (2000
// matrices, 300 reverb frames) for tests/hotfn (--capture).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "hotfn.h"

namespace {

constexpr uint64_t kGotQuatEps = 0x7100000000ull + 0xccb210;   // GOT slot of nn::util::detail::FloatQuaternionEpsilon

int Mode(const char *name) {   // 0 off, 1 on, 2 check
    const char *e = getenv(name);
    if (!e || !*e) return 1;
    if (!strcmp(e, "check")) return 2;
    return e[0] == '0' ? 0 : 1;
}
int ModeDecomp() { static const int m = Mode("SNAP_HOT_MATDECOMP"); return m; }
int ModeDecompS() { static const int m = Mode("SNAP_HOT_MATDECOMP_SCALED"); return m; }
int ModeReverb() { static const int m = Mode("SNAP_HOT_REVERB"); return m; }

// ---- capture of real inputs (u32 tag, u32 size, bytes)
std::mutex g_capMu;
FILE *CaptureFile() {
    static FILE *f = [] {
        const char *p = getenv("SNAP_HOTFN_CAPTURE");
        return p && *p ? fopen(p, "wb") : nullptr;
    }();
    return f;
}
std::atomic<int> g_capLeft[3] = {2000, 2000, 300};
void Capture(int slot, uint32_t tag, std::initializer_list<std::pair<const void *, size_t>> parts) {
    FILE *f = CaptureFile();
    if (!f || g_capLeft[slot].fetch_sub(1) <= 0) return;
    uint32_t size = 0;
    for (auto &p : parts) size += (uint32_t)p.second;
    std::lock_guard<std::mutex> lk(g_capMu);
    fwrite(&tag, 4, 1, f); fwrite(&size, 4, 1, f);
    for (auto &p : parts) fwrite(p.first, 1, p.second, f);
    fflush(f);
}

// ---- check bookkeeping
struct Tally { std::atomic<long> calls{0}, bad{0}; };
Tally g_tally[3];
void Report(int slot, const char *name, bool bad, uint64_t at) {
    const long n = ++g_tally[slot].calls;
    if (bad && ++g_tally[slot].bad <= 5) fprintf(stderr, "[hotfn] MISMATCH %s call %ld (0x%llx)\n", name, n, (unsigned long long)at);
    if ((n & 4095) == 0) fprintf(stderr, "[hotfn] check %s: %ld calls, %ld mismatches\n", name, n, g_tally[slot].bad.load());
}

// pending check of the matrix functions (per thread; the functions do not nest)
struct DecompWant { bool on = false; uint64_t scale = 0, trans = 0; float s[3], q[4], t[3]; };
thread_local DecompWant t_dec[2];

void DecompEntry(Cpu *c, int slot, int mode) {
    const uint64_t outS = c->x[0], outQ = c->x[1], outT = c->x[2], m = c->x[3], sv = c->x[4];
    if (slot == 0) Capture(0, 0x96d390, {{(const void *)(uintptr_t)m, 64}});
    else Capture(1, 0x96d5f0, {{(const void *)(uintptr_t)m, 64}, {(const void *)(uintptr_t)sv, 12}});
    if (mode == 2) {   // native into shadows; the guest runs and DecompCheck compares
        DecompWant &w = t_dec[slot];
        if (slot == 0) hotfn::MatrixDecompose(c, (uint64_t)(uintptr_t)w.s, (uint64_t)(uintptr_t)w.q, (uint64_t)(uintptr_t)w.t, m, kGotQuatEps);
        else hotfn::ScaledMatrixDecompose(c, (uint64_t)(uintptr_t)w.s, (uint64_t)(uintptr_t)w.q, (uint64_t)(uintptr_t)w.t, m, sv, kGotQuatEps);
        w.on = true; w.scale = outS; w.trans = outT;
        c->x[16] = 1;
        return;
    }
    if (slot == 0) hotfn::MatrixDecompose(c, outS, outQ, outT, m, kGotQuatEps);
    else hotfn::ScaledMatrixDecompose(c, outS, outQ, outT, m, sv, kGotQuatEps);
    c->x[16] = 0;   // done: jump to the guest's ret
}

// before the guest's final `stp d0, d1, [x1]`: the quaternion is in d0/d1, scale and translation are stored
void DecompCheck(Cpu *c, int slot, const char *name) {
    DecompWant &w = t_dec[slot];
    if (!w.on) return;
    w.on = false;
    uint64_t q[2] = {(uint64_t)c->v[0], (uint64_t)c->v[1]};
    const bool bad = memcmp(q, w.q, 16) || memcmp((const void *)(uintptr_t)w.scale, w.s, 12) ||
                     memcmp((const void *)(uintptr_t)w.trans, w.t, 12);
    Report(slot, name, bad, w.scale);
}

// pending check of the reverb: copies of everything it writes, run natively at entry
struct ReverbWant {
    bool on = false;
    uint64_t st = 0, out = 0;
    uint32_t n = 0;
    uint8_t state[0x90];
    std::vector<float> out_, buf[4];
};
thread_local ReverbWant t_rev;

}   // namespace

extern "C" void rt_hot_matdecomp(Cpu *c) {
    const int m = ModeDecomp();
    c->x[16] = 1;
    if (m) DecompEntry(c, 0, m);
}
extern "C" void rt_hot_matdecomp_check(Cpu *c) { if (ModeDecomp() == 2) DecompCheck(c, 0, "96d390"); }

extern "C" void rt_hot_matdecomp_s(Cpu *c) {
    const int m = ModeDecompS();
    c->x[16] = 1;
    if (m) DecompEntry(c, 1, m);
}
extern "C" void rt_hot_matdecomp_s_check(Cpu *c) { if (ModeDecompS() == 2) DecompCheck(c, 1, "96d5f0"); }

extern "C" void rt_hot_reverb(Cpu *c) {
    const int m = ModeReverb();
    c->x[16] = 1;
    if (!m) return;
    const uint64_t st = c->x[0], in = c->x[1], out = c->x[3];
    const uint32_t n = (uint32_t)c->x[2];
    uint32_t len[4];
    for (int i = 0; i < 4; i++) len[i] = hotfn::Ld32(st + 0x18 * i);
    if (CaptureFile()) {
        const uint8_t *s = (const uint8_t *)(uintptr_t)st;
        Capture(2, 0x155600, {{&n, 4}, {s, 0x90}, {(const void *)(uintptr_t)in, 4ull * n}, {(const void *)(uintptr_t)out, 4ull * n},
                              {(const void *)(uintptr_t)hotfn::Ld64(st + 8), 4ull * len[0]},
                              {(const void *)(uintptr_t)hotfn::Ld64(st + 0x20), 4ull * len[1]},
                              {(const void *)(uintptr_t)hotfn::Ld64(st + 0x38), 4ull * len[2]},
                              {(const void *)(uintptr_t)hotfn::Ld64(st + 0x50), 4ull * len[3]}});
    }
    if (m == 2) {
        ReverbWant &w = t_rev;
        memcpy(w.state, (const void *)(uintptr_t)st, 0x90);
        w.out_.assign((const float *)(uintptr_t)out, (const float *)(uintptr_t)out + n);
        for (int i = 0; i < 4; i++) {
            const float *b = (const float *)(uintptr_t)hotfn::Ld64(st + 0x18 * i + 8);
            w.buf[i].assign(b, b + len[i]);
            const uint64_t p = (uint64_t)(uintptr_t)w.buf[i].data();
            memcpy(w.state + 0x18 * i + 8, &p, 8);
        }
        hotfn::ReverbFdn4(c, (uint64_t)(uintptr_t)w.state, in, n, (uint64_t)(uintptr_t)w.out_.data());
        w.on = true; w.st = st; w.out = out; w.n = n;
        return;   // the guest runs; rt_hot_reverb_check compares
    }
    hotfn::ReverbFdn4(c, st, in, n, out);
    c->x[16] = 0;
}

// ---- main+0x8b5f10 hierarchy transform and the main+0x8b6750 bone loops
namespace {

int ModeHier() { static const int m = Mode("SNAP_HOT_HIER"); return m; }
int ModeBone() { static const int m = Mode("SNAP_HOT_BONE"); return m; }
Tally g_tally2[3];   // hier, bone loop 1, bone loop 2
void Report2(int slot, const char *name, bool bad, uint64_t at) {
    const long n = ++g_tally2[slot].calls;
    if (bad && ++g_tally2[slot].bad <= 5) fprintf(stderr, "[hotfn] MISMATCH %s call %ld (0x%llx)\n", name, n, (unsigned long long)at);
    if ((n & 4095) == 0) fprintf(stderr, "[hotfn] check %s: %ld calls, %ld mismatches\n", name, n, g_tally2[slot].bad.load());
}

// check mode of the hierarchy walk: its stores are kept here (the guest then runs on unchanged memory)
struct ShadowMem {
    std::vector<std::pair<uint64_t, vf32>> st;
    bool bad = false;
    bool Overlaps(uint64_t a, uint64_t n) {
        for (auto &s : st) if (a < s.first + 16 && s.first < a + n) return true;
        return false;
    }
    vf32 Ld4(uint64_t a) {
        for (auto it = st.rbegin(); it != st.rend(); ++it) {
            if (it->first == a) return it->second;
            if (a < it->first + 16 && it->first < a + 16) { bad = true; break; }
        }
        return hotfn::Ld4(a);
    }
    void St4(uint64_t a, vf32 v) { st.emplace_back(a, v); }
    uint32_t Ld32(uint64_t a) { if (Overlaps(a, 4)) bad = true; return hotfn::Ld32(a); }
    uint64_t Ld64(uint64_t a) { if (Overlaps(a, 8)) bad = true; return hotfn::Ld64(a); }
    uint8_t Ld8(uint64_t a) { if (Overlaps(a, 1)) bad = true; return *(const uint8_t *)(uintptr_t)a; }
};
thread_local int t_hierDepth = 0;
thread_local ShadowMem t_hierWant;

struct BoneWant {
    bool on = false;
    hotfn::BoneLocal b;
    uint64_t sv = 0;
    alignas(16) uint8_t s[16], q[16], t[16], m[64], v[16];
};
thread_local BoneWant t_bone[2];

// x16 is the guest's at entry (the hook protocol's result register is written last). Loop 1 reads x16 after the hook site
// (`add x2, x16, x8`, `mov x26, x16`), so falling through to the guest must leave the guest's x16 there: a non-zero x16
// is its own "fall through"; in the (never seen) case x16 == 0 the native path runs whatever the mode. Loop 2 never reads
// x16 before the bl that ends its region, so 1 is fine there.
void BoneEntry(Cpu *c, int loop) {
    const uint64_t g16 = c->x[16];
    const uint64_t through = loop == 0 ? g16 : 1;
    int mode = ModeBone();
    if (through == 0) mode = 1;
    if (!mode) { c->x[16] = through; return; }
    hotfn::BoneLocal b;
    if (loop == 0) b = hotfn::BoneLoop1(c);
    else if (!hotfn::BoneLoop2(c, b)) { c->x[16] = through; return; }   // parent record at 0: the guest's own path
    if (mode == 2) {
        BoneWant &w = t_bone[loop];
        auto A = [](void *p) { return (uint64_t)(uintptr_t)p; };
        hotfn::BoneFinish(c, b, A(w.m), b.copyScale ? A(w.v) : b.sv, A(w.s), A(w.q), A(w.t), kGotQuatEps);
        w.on = true; w.b = b; w.sv = b.sv;
        c->x[16] = through;
        return;
    }
    if (loop == 0) c->x[26] = c->x[16];   // the guest's `mov x26, x16`
    hotfn::BoneFinish(c, b, b.mat, b.sv, b.outS, b.outQ, b.outT, kGotQuatEps);
    c->x[16] = 0;
}

void BoneCheck(int loop) {
    BoneWant &w = t_bone[loop];
    if (!w.on) return;
    w.on = false;
    const hotfn::BoneLocal &b = w.b;
    auto P = [](uint64_t a) { return (const void *)(uintptr_t)a; };
    bool bad = memcmp(P(b.outS), w.s, 12) || memcmp(P(b.outQ), w.q, 16) || memcmp(P(b.outT), w.t, 12) || memcmp(P(b.mat), w.m, 64);
    if (b.copyScale) bad |= memcmp(P(w.sv), w.v, 12) != 0;
    Report2(1 + loop, loop ? "bone loop 2" : "bone loop 1", bad, b.outS);
}

}   // namespace

extern "C" void rt_hot_hier(Cpu *c) {
    const int m = ModeHier();
    c->x[16] = 1;
    if (!m) return;
    if (m == 2) {   // only the outermost call computes; the guest's own recursion re-enters this hook
        if (t_hierDepth++ == 0) {
            t_hierWant = ShadowMem{};
            hotfn::HierarchyTransform(c, t_hierWant, c->x[0], (uint32_t)c->x[1], c->x[2]);
        }
        return;
    }
    hotfn::DirectMem mem;
    hotfn::HierarchyTransform(c, mem, c->x[0], (uint32_t)c->x[1], c->x[2]);
    c->x[16] = 0;
}
// at the guest's epilogue (main+0x8b66b0): the outermost return compares
extern "C" void rt_hot_hier_check(Cpu *c) {
    (void)c;
    if (ModeHier() != 2 || t_hierDepth == 0 || --t_hierDepth) return;
    ShadowMem &w = t_hierWant;
    if (w.bad) return;   // a load overlapped one of the walk's own stores: not comparable this way
    bool bad = false;
    for (size_t i = 0; i < w.st.size(); i++) {
        bool last = true;
        for (size_t j = i + 1; j < w.st.size(); j++) if (w.st[j].first == w.st[i].first) last = false;
        if (last) bad |= memcmp((const void *)(uintptr_t)w.st[i].first, &w.st[i].second, 16) != 0;
    }
    Report2(0, "hier 8b5f10", bad, w.st.empty() ? 0 : w.st[0].first);
}

extern "C" void rt_hot_bone1(Cpu *c) { BoneEntry(c, 0); }
extern "C" void rt_hot_bone1_check(Cpu *c) { (void)c; if (ModeBone() == 2) BoneCheck(0); }
extern "C" void rt_hot_bone2(Cpu *c) { BoneEntry(c, 1); }
extern "C" void rt_hot_bone2_check(Cpu *c) { (void)c; if (ModeBone() == 2) BoneCheck(1); }

// mismatches seen in check mode so far, all functions (tests/hotfn --hooked)
extern "C" long rt_hotfn_mismatches() {
    return g_tally[0].bad + g_tally[1].bad + g_tally[2].bad + g_tally2[0].bad + g_tally2[1].bad + g_tally2[2].bad;
}

// before the guest's last state store (`st1 {v0.s}[3]`, line 3's filter state still in v0 lane 3)
extern "C" void rt_hot_reverb_check(Cpu *c) {
    if (ModeReverb() != 2 || !t_rev.on) return;
    ReverbWant &w = t_rev;
    w.on = false;
    bool bad = memcmp((const void *)(uintptr_t)w.out, w.out_.data(), 4ull * w.n) != 0;
    for (int i = 0; i < 4; i++) {
        bad |= memcmp((const void *)(uintptr_t)hotfn::Ld64(w.st + 0x18 * i + 8), w.buf[i].data(), 4 * w.buf[i].size()) != 0;
        bad |= hotfn::Ld32(w.st + 0x18 * i + 0x10) != hotfn::Ld32((uint64_t)(uintptr_t)w.state + 0x18 * i + 0x10);
    }
    bad |= memcmp((const void *)(uintptr_t)(w.st + 0x60), w.state + 0x60, 0x84 - 0x60) != 0;   // lines 0-2 state, coefficients
    const uint32_t lane3 = (uint32_t)(c->v[0] >> 96);
    bad |= lane3 != hotfn::Ld32((uint64_t)(uintptr_t)w.state + 0x84);
    Report(2, "155600", bad, w.st);
}
