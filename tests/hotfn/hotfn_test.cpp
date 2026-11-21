// Differential test and microbenchmark for host/mods/hotfn.h (decomp-for-speed pass, 2026-11-20; project code, ours).
//
// Runs each generated guest function (extracted from the generated main by tests/hotfn/run_hotfn_test.py into
// build/hotfn/guest_fns.c, never committed) and its native version from hotfn.h on the same inputs and requires
// bit-exact results: every byte of the memory the function can touch (outputs, state, guard bytes around them),
// the FPCR/FPSR words, and the registers the native version promises to leave as the guest does. Inputs: random bit
// patterns (NaN, infinity, denormal included), random floats over a wide exponent range, realistic data (TRS matrices,
// audio), and, if a capture file from a game run is given (SNAP_HOTFN_CAPTURE), the real inputs recorded there. Every
// case runs under four FPCR settings (default, FZ, DN, FZ|DN) and some under directed rounding.
//
// The guest functions read the main image (constant tables, GOT slots): the test maps the PT_LOAD segments of the
// main ELF (argv[1], the local analysis/import/v262144-main.elf) at the guest base, as the game does.
//
// Usage: hotfn_test.exe <main.elf> [cases per function] [capture file]
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../../host/mods/hotfn.h"

extern "C" {   // build/hotfn/guest_fns.c (also defines g_fnprof and fnprof_hit for the generated code)
void f_710096d390(Cpu *c);
void f_710096d5f0(Cpu *c);
void f_7100155600(Cpu *c);
void f_71008b5f10(Cpu *c);
// build/hotfn/regions.c (tests/recomp/hotfn_hook_test.py --emit): the two bone-loop bodies of main+0x8b6750 that
// rt_hot_bone1 / rt_hot_bone2 replace, as functions ending at the hook's target
void rg_8b7400(Cpu *c);
void rg_8b7ac0(Cpu *c);
}   // (the regions end by dispatching to their target: guest_fns.c's rt_dispatch stub returns)
#ifdef HOTFN_HOOKED
// --hooked: the "native" side is the generated function with the HOTFN hooks (build/hotfn/hooked_fns.c from
// tests/recomp/hotfn_hook_test.py --emit) running host/mods/hotfn.cpp, as in the game; SNAP_HOT_* pick the mode
extern "C" {
void hk_96d390(Cpu *c);
void hk_96d5f0(Cpu *c);
void hk_155600(Cpu *c);
void hk_8b5f10(Cpu *c);
void hkrg_8b7400(Cpu *c);
void hkrg_8b7ac0(Cpu *c);
long rt_hotfn_mismatches();
void rt_inline_hook(Cpu *c, uint64_t ra, void (*fn)(Cpu *c)) { (void)ra; fn(c); }
}
#endif

namespace {

constexpr uint64_t kBase = 0x7100000000ull;
constexpr uint64_t kGotQuatEps = kBase + 0xccb210;

bool MapMain(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return false; }
    std::vector<uint8_t> elf;
    fseek(f, 0, SEEK_END); elf.resize((size_t)ftell(f)); fseek(f, 0, SEEK_SET);
    fread(elf.data(), 1, elf.size(), f); fclose(f);
    uint64_t phoff; uint16_t phnum;
    memcpy(&phoff, &elf[0x20], 8); memcpy(&phnum, &elf[0x38], 2);
    uint64_t hi = 0;
    for (int i = 0; i < phnum; i++) {
        uint32_t type; uint64_t va, msz;
        memcpy(&type, &elf[phoff + 56 * i], 4); memcpy(&va, &elf[phoff + 56 * i + 16], 8); memcpy(&msz, &elf[phoff + 56 * i + 40], 8);
        if (type == 1) hi = std::max(hi, va + msz);
    }
    hi = (hi + 0xffff) & ~0xffffull;
    uint8_t *img = (uint8_t *)VirtualAlloc((void *)kBase, hi, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (img != (uint8_t *)kBase) { fprintf(stderr, "cannot map the main image at 0x%llx\n", (unsigned long long)kBase); return false; }
    for (int i = 0; i < phnum; i++) {
        uint32_t type; uint64_t off, va, fsz;
        memcpy(&type, &elf[phoff + 56 * i], 4); memcpy(&off, &elf[phoff + 56 * i + 8], 8);
        memcpy(&va, &elf[phoff + 56 * i + 16], 8); memcpy(&fsz, &elf[phoff + 56 * i + 32], 8);
        if (type == 1) memcpy(img + va, &elf[off], fsz);
    }
    // GOT slot of nn::util::detail::FloatQuaternionEpsilon (an sdk import): point it at the sdk's value, 1e-5f
    static float eps = 9.99999974738e-06f;
    uint64_t p = (uint64_t)(uintptr_t)&eps;
    memcpy((void *)kGotQuatEps, &p, 8);
    return true;
}

Cpu *NewCpu() {
    Cpu *c = (Cpu *)_aligned_malloc(sizeof(Cpu), 64);
    memset(c, 0, sizeof(Cpu));
    return c;
}

struct Rng {
    std::mt19937_64 g;
    explicit Rng(uint64_t s) : g(s) {}
    uint32_t u32() { return (uint32_t)g(); }
    double uni() { return (g() >> 11) * (1.0 / 9007199254740992.0); }
    float range(float a, float b) { return (float)(a + (b - a) * uni()); }
    // a float with a wide exponent range and some special values
    float wild() {
        switch (g() % 16) {
        case 0: { uint32_t u = u32(); float f; memcpy(&f, &u, 4); return f; }              // any bit pattern
        case 1: return 0.0f;
        case 2: return -0.0f;
        case 3: { uint32_t u = (u32() & 0x807fffffu); float f; memcpy(&f, &u, 4); return f; }   // denormal
        case 4: return (g() & 1) ? INFINITY : -INFINITY;
        case 5: { uint32_t u = 0x7f800001u | (u32() & 0x807fffffu); float f; memcpy(&f, &u, 4); return f; }   // NaN
        case 6: return (g() & 1) ? 1.17549435e-38f : -1.17549435e-38f;                 // smallest normal
        default: return (float)(range(-1, 1) * std::ldexp(1.0, (int)(g() % 80) - 40));
        }
    }
};

const uint64_t kFpcrModes[] = {0, FPCR_FZ, FPCR_DN, FPCR_FZ | FPCR_DN, 3u << 22, (1u << 22) | FPCR_FZ};

struct Totals { long cases = 0, bad = 0; };

void RunPair(Cpu *cg, Cpu *cn, void (*guest)(Cpu *), void (*native)(Cpu *), uint64_t fpcr) {
    cg->fpcr = fpcr; cn->fpcr = fpcr;
    const unsigned old = _mm_getcsr();
    rt_fpcr_sync(cg); guest(cg);
    rt_fpcr_sync(cn); native(cn);
    _mm_setcsr(old);
}

// ---------------------------------------------------------------- main+0x96d390 matrix decompose

#ifdef HOTFN_HOOKED
void Native96d390(Cpu *c) { hk_96d390(c); }
void Native96d5f0(Cpu *c) { hk_96d5f0(c); }
#else
void Native96d390(Cpu *c) { hotfn::MatrixDecompose(c, c->x[0], c->x[1], c->x[2], c->x[3], kGotQuatEps); }
void Native96d5f0(Cpu *c) { hotfn::ScaledMatrixDecompose(c, c->x[0], c->x[1], c->x[2], c->x[3], c->x[4], kGotQuatEps); }
#endif

void RandomRotation(Rng &r, float m[3][3]) {
    double q[4], n = 0;
    for (double &x : q) { x = r.range(-1, 1); n += x * x; }
    n = std::sqrt(n);
    if (n < 1e-6) { q[0] = 1; n = 1; q[1] = q[2] = q[3] = 0; }
    const double x = q[0] / n, y = q[1] / n, z = q[2] / n, w = q[3] / n;
    const double R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)},
                            {2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)},
                            {2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)}};
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m[i][j] = (float)R[i][j];
}

// one input matrix (16 floats, rows of 4) of the given kind
void MakeMatrix96d390(Rng &r, int kind, float out[16]) {
    if (kind == 0) { for (int i = 0; i < 16; i++) { uint32_t u = r.u32(); memcpy(&out[i], &u, 4); } return; }
    if (kind == 1) { for (int i = 0; i < 16; i++) out[i] = r.wild(); return; }
    float R[3][3];
    RandomRotation(r, R);
    float s[3];
    for (float &x : s) x = std::exp(r.range(-6, 5)) * ((r.g() % 8) ? 1.f : -1.f);
    if (r.g() % 4 == 0) s[1] = s[2] = s[0];                         // uniform scale
    if (r.g() % 32 == 0) s[r.g() % 3] = 0;                           // a zero axis
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) out[4 * i + j] = R[i][j] * s[i];
        out[4 * i + 3] = (r.g() % 8) ? 0.f : r.range(-1, 1);         // the guest reads lane 3 too
    }
    for (int j = 0; j < 3; j++) out[12 + j] = r.range(-500, 500);
    out[15] = (r.g() % 2) ? 1.f : 0.f;
    if (kind == 3) out[r.g() % 16] = r.wild();                       // one odd value in a realistic matrix
}

// sv: the scale vector of 0x96d5f0 (x4), null for 0x96d390
void CaseDecompose(Cpu *cg, Cpu *cn, const float mat[16], const float *sv, uint64_t fpcr, uint64_t seed, Totals &t) {
    alignas(64) static uint8_t ag[256], an[256];
    std::mt19937_64 g(seed);
    for (int i = 0; i < 256; i += 8) { uint64_t v = g(); memcpy(ag + i, &v, 8); }
    memcpy(ag + 16, mat, 64);   // matrix at +16, scale vector at +80, scale out at +96, quat at +128, translation at +176
    if (sv) memcpy(ag + 80, sv, 12);
    memcpy(an, ag, 256);
    for (Cpu *c : {cg, cn}) {
        uint8_t *a = c == cg ? ag : an;
        c->x[0] = (uint64_t)(uintptr_t)(a + 96); c->x[1] = (uint64_t)(uintptr_t)(a + 128);
        c->x[2] = (uint64_t)(uintptr_t)(a + 176); c->x[3] = (uint64_t)(uintptr_t)(a + 16);
        c->x[4] = (uint64_t)(uintptr_t)(a + 80);
        c->fpsr = 0;
    }
    if (sv) RunPair(cg, cn, f_710096d5f0, Native96d5f0, fpcr);
    else RunPair(cg, cn, f_710096d390, Native96d390, fpcr);
    t.cases++;
    if (memcmp(ag, an, 256) || cg->fpsr != cn->fpsr) {
        if (++t.bad <= 8) {
            fprintf(stderr, "  MISMATCH %s fpcr=%llx\n   in:", sv ? "96d5f0" : "96d390", (unsigned long long)fpcr);
            for (int i = 0; i < 16; i++) { uint32_t u; memcpy(&u, &mat[i], 4); fprintf(stderr, " %08x", u); }
            for (int i = 0; sv && i < 3; i++) { uint32_t u; memcpy(&u, &sv[i], 4); fprintf(stderr, " s%08x", u); }
            fprintf(stderr, "\n");
            for (int off : {96, 128, 176}) {
                fprintf(stderr, "   +%d guest", off);
                for (int i = 0; i < 4; i++) { uint32_t u; memcpy(&u, ag + off + 4 * i, 4); fprintf(stderr, " %08x", u); }
                fprintf(stderr, " native");
                for (int i = 0; i < 4; i++) { uint32_t u; memcpy(&u, an + off + 4 * i, 4); fprintf(stderr, " %08x", u); }
                fprintf(stderr, "\n");
            }
        }
    }
}

// ---------------------------------------------------------------- main+0x155600 reverb delay network

#ifdef HOTFN_HOOKED
void Native155600(Cpu *c) { hk_155600(c); }
#else
void Native155600(Cpu *c) { hotfn::ReverbFdn4(c, c->x[0], c->x[1], (uint32_t)c->x[2], c->x[3]); }
#endif

struct FdnInput {
    uint32_t n = 0;
    uint8_t st[0x90] = {};
    std::vector<float> in, out, buf[4];
};

// a delay network state of the given kind: 0 tiny lines and any bit patterns, 1 short lines and audio values,
// 2 short lines and wide-range values, 3 realistic (reverb-length lines, 1024-sample frames)
FdnInput MakeFdn(Rng &r, int kind) {
    FdnInput f;
    const uint32_t maxLen = kind == 0 ? 9 : kind == 3 ? 4000 : 300;
    const uint32_t minLen = kind == 3 ? 1000 : 1;
    f.n = kind == 3 ? 1024 : (uint32_t)(r.g() % (kind == 0 ? 40 : 700));
    auto val = [&]() { return kind == 0 ? [&] { uint32_t u = r.u32(); float x; memcpy(&x, &u, 4); return x; }() : kind == 2 ? r.wild() : r.range(-1, 1); };
    for (int i = 0; i < 4; i++) {
        const uint32_t len = minLen + (uint32_t)(r.g() % (maxLen - minLen + 1));
        const uint32_t pos = (r.g() % 16 == 0) ? len : (uint32_t)(r.g() % len);
        memcpy(f.st + 0x18 * i, &len, 4);
        memcpy(f.st + 0x18 * i + 0x10, &pos, 4);
        uint32_t pad = r.u32(); memcpy(f.st + 0x18 * i + 4, &pad, 4);
        f.buf[i].resize(len);
        for (float &x : f.buf[i]) x = val();
        float bco = kind == 0 || kind == 2 ? val() : r.range(0.05f, 1.0f);
        float aco = kind == 0 || kind == 2 ? val() : bco - 1.0f;
        float y = val();
        memcpy(f.st + 0x60 + 12 * i, &y, 4); memcpy(f.st + 0x64 + 12 * i, &bco, 4); memcpy(f.st + 0x68 + 12 * i, &aco, 4);
    }
    f.in.resize(f.n); f.out.resize(f.n);
    for (float &x : f.in) x = val();
    for (float &x : f.out) x = val();
    return f;
}

// lays the input out in one arena (state, delay lines, input, output, random guard bytes between), runs both sides
// on copies and compares every byte of the arenas
void Case155600(Cpu *cg, Cpu *cn, const FdnInput &f, uint64_t fpcr, uint64_t seed, Totals &t) {
    size_t off[7], at = 0x90 + 64;
    const std::vector<float> *parts[6] = {&f.buf[0], &f.buf[1], &f.buf[2], &f.buf[3], &f.in, &f.out};
    for (int i = 0; i < 6; i++) { off[i] = at; at += (parts[i]->size() * 4 + 64 + 15) & ~(size_t)15; }
    off[6] = at;
    static std::vector<uint8_t> ag, an;
    ag.assign(at, 0);
    std::mt19937_64 g(seed);
    for (size_t i = 0; i + 8 <= at; i += 8) { uint64_t v = g(); memcpy(&ag[i], &v, 8); }
    memcpy(ag.data(), f.st, 0x90);
    for (int i = 0; i < 6; i++) if (!parts[i]->empty()) memcpy(&ag[off[i]], parts[i]->data(), parts[i]->size() * 4);
    an = ag;
    for (Cpu *c : {cg, cn}) {
        uint8_t *a = c == cg ? ag.data() : an.data();
        for (int i = 0; i < 4; i++) { uint64_t p = (uint64_t)(uintptr_t)(a + off[i]); memcpy(a + 0x18 * i + 8, &p, 8); }
        c->x[0] = (uint64_t)(uintptr_t)a; c->x[1] = (uint64_t)(uintptr_t)(a + off[4]);
        c->x[2] = f.n | (uint64_t)g() << 32;   // w2: the upper half is not part of the argument
        c->x[3] = (uint64_t)(uintptr_t)(a + off[5]);
        c->fpsr = 0;
    }
    cn->x[2] = cg->x[2];
    RunPair(cg, cn, f_7100155600, Native155600, fpcr);
    t.cases++;
    // the arenas hold their own addresses (the delay-line pointers): compare those slots by offset, the rest by value
    for (int i = 0; i < 4; i++) { memset(ag.data() + 0x18 * i + 8, 0, 8); memset(an.data() + 0x18 * i + 8, 0, 8); }
    if (ag != an || cg->fpsr != cn->fpsr) {
        if (++t.bad <= 8) {
            size_t first = 0;
            while (first < at && ag[first] == an[first]) first++;
            fprintf(stderr, "  MISMATCH 155600 fpcr=%llx n=%u: first differing byte at +0x%zx (state 0x90, lines at 0x%zx 0x%zx 0x%zx 0x%zx, in 0x%zx, out 0x%zx)\n",
                    (unsigned long long)fpcr, f.n, first, off[0], off[1], off[2], off[3], off[4], off[5]);
        }
    }
}

// ---------------------------------------------------------------- main+0x8b5f10 hierarchy walk, main+0x8b6750 bone loops

// a random 4x4 matrix: kind 0 any bits, 1 wide-range values, 2 a TRS-like transform, 3 singular (two equal rows),
// 4 a third of the values NaN with distinct payloads (catches operand-order slips: ARM returns the first NaN)
void MakeMat(Rng &r, int kind, float m[16]) {
    if (kind == 4) {
        for (int i = 0; i < 16; i++) {
            if (r.g() % 3) { m[i] = r.range(-2, 2); continue; }
            const uint32_t u = 0x7f800001u | (r.u32() & 0x803ffffeu) | (r.g() & 1 ? 0x400000u : 0);
            memcpy(&m[i], &u, 4);
        }
        return;
    }
    if (kind >= 2) {
        MakeMatrix96d390(r, 2, m);
        if (kind == 3) memcpy(m + 4, m, 16);
        return;
    }
    for (int i = 0; i < 16; i++) m[i] = kind == 0 ? [&] { uint32_t u = r.u32(); float x; memcpy(&x, &u, 4); return x; }() : r.wild();
}

// One arena holds the whole world either side sees; both get identical copies at the same offsets from different bases,
// and every pointer inside is written relative to the base (so the arenas compare byte for byte after the pointers are
// made relative again).
struct Arena {
    std::vector<uint8_t> a;
    std::vector<size_t> ptrs;   // offsets of 64-bit pointer slots (relative pointers in the template)
    size_t top = 0;
    explicit Arena(size_t n) : a(n, 0) {}
    size_t alloc(size_t n) { size_t o = (top + 15) & ~(size_t)15; top = o + n; return o; }
    void u32(size_t o, uint32_t v) { memcpy(&a[o], &v, 4); }
    void u64(size_t o, uint64_t v) { memcpy(&a[o], &v, 8); }
    void ptr(size_t o, size_t target) { u64(o, target); ptrs.push_back(o); }
    void f(size_t o, const float *v, int n) { memcpy(&a[o], v, 4 * n); }
};

// copies the template to a real buffer with absolute pointers; back to relative for the comparison
void Place(const Arena &t, std::vector<uint8_t> &buf) {
    buf = t.a;
    const uint64_t base = (uint64_t)(uintptr_t)buf.data();
    for (size_t o : t.ptrs) { uint64_t v; memcpy(&v, &buf[o], 8); v += base; memcpy(&buf[o], &v, 8); }
}
void Unplace(const Arena &t, std::vector<uint8_t> &buf) {
    const uint64_t base = (uint64_t)(uintptr_t)buf.data();
    for (size_t o : t.ptrs) { uint64_t v; memcpy(&v, &buf[o], 8); v -= base; memcpy(&buf[o], &v, 8); }
}

constexpr size_t kStack = 1 << 20;
uint8_t *StackFor(Cpu *c) {
    static uint8_t *sg = (uint8_t *)_aligned_malloc(kStack, 64), *sn = (uint8_t *)_aligned_malloc(kStack, 64);
    return c->x[31] ? sn : sg;   // x[31] tags the native-side Cpu (set in main)
}

#ifdef HOTFN_HOOKED
void Native8b5f10(Cpu *c) { hk_8b5f10(c); }
void NativeBone1(Cpu *c) { hkrg_8b7400(c); }
void NativeBone2(Cpu *c) { hkrg_8b7ac0(c); }
#else
void Native8b5f10(Cpu *c) { hotfn::DirectMem m; hotfn::HierarchyTransform(c, m, c->x[0], (uint32_t)c->x[1], c->x[2]); }
void NativeBone1(Cpu *c) {
    const hotfn::BoneLocal b = hotfn::BoneLoop1(c);
    c->x[26] = c->x[16];
    hotfn::BoneFinish(c, b, b.mat, b.sv, b.outS, b.outQ, b.outT, kGotQuatEps);
}
void NativeBone2(Cpu *c) {
    hotfn::BoneLocal b;
    if (hotfn::BoneLoop2(c, b)) hotfn::BoneFinish(c, b, b.mat, b.sv, b.outS, b.outQ, b.outT, kGotQuatEps);
}
#endif

// compares the arenas, the frames (sp .. sp+0x400) and the callee-saved registers; prints the first difference
void CompareWorld(const char *name, const Arena &t, std::vector<uint8_t> &ag, std::vector<uint8_t> &an, Cpu *cg, Cpu *cn,
                  uint64_t fpcr, Totals &tot) {
    Unplace(t, ag); Unplace(t, an);
    const uint8_t *fg = (const uint8_t *)(uintptr_t)cg->sp, *fn = (const uint8_t *)(uintptr_t)cn->sp;
    bool regs = cg->fpsr != cn->fpsr;
    auto rel = [](uint64_t v, const std::vector<uint8_t> &b) {   // arena pointers compare by offset
        const uint64_t lo = (uint64_t)(uintptr_t)b.data();
        return v - lo < b.size() ? v - lo : v;
    };
    for (int r = 19; r <= 28; r++) regs |= rel(cg->x[r], ag) != rel(cn->x[r], an);   // callee-saved (x26: bone loop 1's output)
    for (int r = 8; r <= 15; r++) regs |= (uint64_t)cg->v[r] != (uint64_t)cn->v[r];
    tot.cases++;
    const bool frame = memcmp(fg, fn, 0x400) != 0;
    if (ag != an || frame || regs) {
        if (++tot.bad <= 8) {
            size_t first = 0;
            while (first < ag.size() && ag[first] == an[first]) first++;
            fprintf(stderr, "  MISMATCH %s fpcr=%llx: arena %s (first +0x%zx) frame %s regs %s (fpsr %llx/%llx)\n", name,
                    (unsigned long long)fpcr, ag != an ? "differs" : "same", first, frame ? "differs" : "same", regs ? "differ" : "same",
                    (unsigned long long)cg->fpsr, (unsigned long long)cn->fpsr);
        }
    }
}

// a skeleton for the walk: n nodes, random tree, index tables and the matrix-address scheme of main+0x8b5f10
struct HierWorld { Arena t{1 << 16}; size_t skel, ctx; uint32_t w1; };
HierWorld MakeHier(Rng &r, int kind) {
    HierWorld w;
    Arena &t = w.t;
    t.alloc(64);   // keep offset 0 unused
    const int n = 1 + (int)(r.g() % 24);
    const size_t skel = t.alloc(16), P = t.alloc(0xc0), T = t.alloc(8 * n), rootE = t.alloc(16);
    const size_t ctx = t.alloc(0x30), X10 = t.alloc(16), X11 = t.alloc(0x50), map = t.alloc(4 * n);
    const size_t O = t.alloc(0x40), Q = t.alloc(16), X12 = t.alloc(0xa00), X13 = t.alloc(0x30), X11b = t.alloc(0x40), b858 = t.alloc(16);
    const size_t A = t.alloc(64), B = t.alloc(64), C = t.alloc(64);
    const size_t nodes = t.alloc(0x40 * n), entries = t.alloc(16 * n);
    const bool flag = r.g() & 1;
    const uint32_t off = 16 * (uint32_t)(r.g() % 4), stride = 64 + 16 * (uint32_t)(r.g() % 2), b858v = 16 * (uint32_t)(r.g() % 4);
    const size_t mats = t.alloc(64 + (size_t)stride * n + 64);
    w.skel = skel; w.ctx = ctx;
    t.ptr(skel, P); t.ptr(P + 0x48, T); t.ptr(P + 0xb0, rootE);
    t.ptr(ctx, X10); t.ptr(ctx + 8, X11); t.ptr(ctx + 0x18, A); t.ptr(ctx + 0x20, B); t.ptr(ctx + 0x28, C);
    t.u64(X11 + 0x48, n); t.ptr(X11 + 0x40, map);
    t.ptr(X10, O); t.ptr(O, mats); t.ptr(O + 0x18, Q); t.u64(Q, 0x1234); t.u64(O + 0x20, 0x1234);
    t.ptr(O + 0x10, X12); t.ptr(O + 0x30, X13);
    t.u64(X12 + 0x48, n); t.a[X12 + 0x999] = flag; t.ptr(X12 + 0x858, b858); t.u32(b858, b858v);
    t.u64(X13 + 0x20, 1); t.ptr(X13 + 0x18, X11b); t.u32(X11b, off); t.u32(X11b + 0x34, stride); t.u32(X13 + 0x10, stride);
    std::vector<int> perm(n);
    for (int i = 0; i < n; i++) perm[i] = i;
    for (int i = n - 1; i > 0; i--) std::swap(perm[i], perm[r.g() % (i + 1)]);
    for (int i = 0; i < n; i++) t.u32(map + 4 * i, perm[i]);
    // tree: node i's parent is a lower node or the root list; children linked through +0x20 (pushed at the front)
    std::vector<size_t> head(n + 1, 0);   // head[n] = root list
    for (int i = 0; i < n; i++) {
        const int parent = i == 0 || r.g() % 3 == 0 ? n : (int)(r.g() % i);
        const size_t nd = nodes + 0x40 * i;
        t.u32(nd + 0x30, i);
        if (head[parent]) t.ptr(nd + 0x20, head[parent]);
        head[parent] = nd;
    }
    for (int i = 0; i < n; i++) {
        if (r.g() % 8 == 0) continue;   // no entry: the walk stops there
        t.ptr(T + 8 * i, entries + 16 * i);
        if (head[i]) t.ptr(entries + 16 * i + 8, head[i]);
    }
    if (head[n]) t.ptr(rootE + 8, head[n]);
    t.u32(ctx + 0x10, r.g() % 4 == 0 ? (uint32_t)(r.g() % n) : 0xffffffffu);   // a node to skip
    w.w1 = r.g() % 3 == 0 ? (uint32_t)(r.g() % n) : 0xffffffffu;
    float m[16];
    for (size_t M : {A, B, C}) { MakeMat(r, kind == 4 && M != C ? 2 : kind, m); t.f(M, m, 16); }   // kind 4: NaNs only where order shows
    for (int i = 0; i < n; i++) {
        const uint32_t idx = perm[i];
        const size_t d = flag ? (uint32_t)(stride * idx + b858v) : (size_t)stride * idx + off;
        MakeMat(r, kind == 3 ? 2 : kind, m); t.f(mats + d, m, 16);
    }
    return w;
}

void CaseHier(Cpu *cg, Cpu *cn, const HierWorld &w, uint64_t fpcr, Totals &tot) {
    static std::vector<uint8_t> ag, an;
    Place(w.t, ag); Place(w.t, an);
    for (Cpu *c : {cg, cn}) {
        std::vector<uint8_t> &a = c == cg ? ag : an;
        const uint64_t base = (uint64_t)(uintptr_t)a.data();
        c->x[0] = base + w.skel; c->x[1] = w.w1; c->x[2] = base + w.ctx;
        c->sp = (uint64_t)(uintptr_t)(StackFor(c) + kStack - 0x800);
        memset((void *)(uintptr_t)c->sp, 0, 0x400);
        c->fpsr = 0;
    }
    RunPair(cg, cn, f_71008b5f10, Native8b5f10, fpcr);
    CompareWorld("8b5f10", w.t, ag, an, cg, cn, fpcr, tot);
}

// the bone loops: matrices, records, output arrays and a frame
struct BoneWorld { Arena t{1 << 14}; size_t mats, flags, outS, outQ, outT, root, svs, recs, out2; int n; };
BoneWorld MakeBone(Rng &r, int kind) {
    BoneWorld w;
    Arena &t = w.t;
    t.alloc(64);
    const int n = w.n = 2 + (int)(r.g() % 6);
    w.mats = t.alloc(64 * n); w.flags = t.alloc(4 * n); w.outS = t.alloc(16 * n); w.outQ = t.alloc(16 * n); w.outT = t.alloc(16 * n);
    w.root = t.alloc(0x50); w.svs = t.alloc(16 * n); w.recs = t.alloc(0x60 * n); w.out2 = t.alloc(0x40);
    float m[16];
    std::mt19937_64 g(r.g());
    for (size_t i = 0; i < t.a.size(); i += 8) { uint64_t v = g(); memcpy(&t.a[i], &v, 8); }   // outputs start as noise
    for (int i = 0; i < n; i++) {
        MakeMat(r, kind, m); t.f(w.mats + 64 * i, m, 16);
        t.u32(w.flags + 4 * i, r.g() & 1);
        float s[4] = {r.wild(), r.wild(), r.wild(), r.wild()};
        if (kind >= 2) for (float &x : s) x = std::exp(r.range(-2, 2));
        t.f(w.svs + 16 * i, s, 4);
        MakeMat(r, kind, m); t.f(w.recs + 0x60 * i, m, 16);
        t.f(w.recs + 0x60 * i + 0x40, s, 4);
        t.u32(w.recs + 0x60 * i + 0x50, r.g() & 1);
    }
    MakeMat(r, kind == 3 ? 2 : kind, m); t.f(w.root + 0x10, m, 16);
    return w;
}

void SetFrame(Cpu *c, uint64_t svs) {
    c->sp = (uint64_t)(uintptr_t)(StackFor(c) + kStack - 0x2000);
    uint8_t *f = (uint8_t *)(uintptr_t)c->sp;
    memset(f, 0, 0x400);
    const float sa[4] = {1, -1, 1, -1}, sb[4] = {-1, 1, -1, 1};
    memcpy(f + 8, &svs, 8);
    memcpy(f + 0x10, sb, 16); memcpy(f + 0x20, sa, 16);   // loop 1
    memcpy(f + 0x40, sb, 16); memcpy(f + 0x50, sa, 16);   // loop 2
    c->x[29] = c->sp + 0x200;
    // the TBL index registers the loops load before they start (callee-saved d8-d12)
    c->v[8] = 0x0302010003020100ull; c->v[9] = 0x0b0a09080b0a0908ull; c->v[10] = 0x0706050407060504ull;
    c->v[11] = 0x0f0e0d0c0f0e0d0cull; c->v[12] = 0x0b0a09080f0e0d0cull;
}

void CaseBone(Cpu *cg, Cpu *cn, const BoneWorld &w, int loop, Rng &r, uint64_t fpcr, Totals &tot) {
    static std::vector<uint8_t> ag, an;
    Place(w.t, ag); Place(w.t, an);
    const uint32_t child = (uint32_t)(r.g() % w.n), parent = r.g() % 4 == 0 ? 0xffffffffu : (uint32_t)(r.g() % w.n);
    const uint32_t pidx = (uint32_t)(r.g() % w.n);
    const bool noFlags = r.g() % 4 == 0;
    const uint64_t noise = r.g();
    for (Cpu *c : {cg, cn}) {
        const uint64_t base = (uint64_t)(uintptr_t)(c == cg ? ag : an).data();
        for (int i = 0; i < 31; i++) if (i != 29) c->x[i] = noise * (i + 1);   // registers the body must not depend on
        SetFrame(c, base + w.svs);
        if (loop == 0) {
            c->x[8] = child | (noise << 32); c->x[11] = (uint32_t)(r.g() % w.n) | (noise & ~0xffffffffull);
            c->x[9] = parent; c->x[10] = pidx;
            c->x[14] = base + w.mats; c->x[15] = noFlags ? 0 : base + w.flags;
            c->x[16] = base + w.outT; c->x[25] = base + w.outQ; c->x[17] = base + w.outS;
            c->x[20] = base + w.root;   // root matrix at x20+0x10
        } else {
            c->x[0] = base + w.out2; c->x[9] = child; c->x[10] = parent; c->x[11] = pidx;
            c->x[24] = base + w.recs; c->x[27] = 0x60; c->x[20] = base + w.root;
            c->x[8] = c->x[24] + 0x60ull * child;   // the loop head's UMADDL (the only way in)
        }
        c->fpsr = 0;
    }
    // x11 for loop 1 must be the same on both sides (drawn once above per Cpu: redo for the native one)
    cn->x[11] = cg->x[11];
    RunPair(cg, cn, loop ? rg_8b7ac0 : rg_8b7400, loop ? NativeBone2 : NativeBone1, fpcr);
    for (Cpu *c : {cg, cn}) memset((void *)(uintptr_t)(c->sp + 8), 0, 8);   // the scale-array pointer differs by side
    CompareWorld(loop ? "bone loop 2" : "bone loop 1", w.t, ag, an, cg, cn, fpcr, tot);
}

// ---------------------------------------------------------------- capture file (real inputs from a game run)
// Records of hotfn.cpp's SNAP_HOTFN_CAPTURE: u32 function tag, u32 byte count, then the bytes.
struct Capture { uint32_t tag; std::vector<uint8_t> bytes; };

std::vector<Capture> LoadCapture(const char *path) {
    std::vector<Capture> out;
    FILE *f = path ? fopen(path, "rb") : nullptr;
    if (!f) return out;
    for (;;) {
        uint32_t h[2];
        if (fread(h, 4, 2, f) != 2) break;
        Capture c{h[0], std::vector<uint8_t>(h[1])};
        if (h[1] && fread(c.bytes.data(), 1, h[1], f) != h[1]) break;
        out.push_back(std::move(c));
    }
    fclose(f);
    return out;
}

// Thread cycles per call (QueryThreadCycleTime: time on the CPU only, so other load on a shared PC does not count as
// ours). Guest and native alternate over 21 rounds; the median round counts.
template <class G, class N> void Bench(const char *name, G guest, N native, int reps) {
    std::vector<double> tg, tn;
    auto cyc = [] { ULONG64 v; QueryThreadCycleTime(GetCurrentThread(), &v); return v; };
    for (int round = 0; round < 21; round++) {
        ULONG64 a = cyc();
        for (int i = 0; i < reps; i++) guest(i);
        ULONG64 b = cyc();
        for (int i = 0; i < reps; i++) native(i);
        ULONG64 e = cyc();
        tg.push_back(double(b - a) / reps); tn.push_back(double(e - b) / reps);
    }
    std::sort(tg.begin(), tg.end()); std::sort(tn.begin(), tn.end());
    printf("%s bench: guest %.0f cycles/call, native %.0f cycles/call, %.2fx\n", name, tg[10], tn[10], tg[10] / tn[10]);
}

}   // namespace

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: hotfn_test <main.elf> [cases] [capture]\n"); return 2; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    if (!MapMain(argv[1])) return 2;
    const long cases = argc > 2 ? atol(argv[2]) : 20000;
    const std::vector<Capture> cap = LoadCapture(argc > 3 ? argv[3] : nullptr);
    Cpu *cg = NewCpu(), *cn = NewCpu();
    cn->x[31] = 1;   // x[31] is unused by the code: tags the native side for StackFor
    int fail = 0;

    for (int variant = 0; variant < 2; variant++) {   // main+0x96d390, then main+0x96d5f0 (with a scale vector)
        const bool scaled = variant == 1;
        const uint32_t tag = scaled ? 0x96d5f0 : 0x96d390;
        Totals t;
        Rng r(tag);
        float m[16], sv[3];
        auto makeScale = [&](int kind) {
            for (float &x : sv) x = kind < 2 ? r.wild() : (r.g() % 8 ? std::exp(r.range(-3, 3)) : r.range(-2, 2));
            if (kind >= 2 && r.g() % 4 == 0) sv[1] = sv[2] = sv[0];
        };
        for (long i = 0; i < cases; i++) {
            MakeMatrix96d390(r, (int)(i % 4), m);
            makeScale((int)(i % 4));
            for (uint64_t mode : kFpcrModes) CaseDecompose(cg, cn, m, scaled ? sv : nullptr, mode, r.g(), t);
        }
        long real = 0;
        for (const Capture &k : cap)
            if (k.tag == tag && k.bytes.size() == (scaled ? 76u : 64u)) {
                memcpy(m, k.bytes.data(), 64); if (scaled) memcpy(sv, k.bytes.data() + 64, 12);
                real++;
                for (uint64_t mode : kFpcrModes) CaseDecompose(cg, cn, m, scaled ? sv : nullptr, mode, real, t);
            }
        printf("%x %s: %ld cases (%ld captured inputs x %zu FPCR modes), %ld mismatches\n", tag,
               scaled ? "ScaledMatrixDecompose" : "MatrixDecompose", t.cases, real, sizeof kFpcrModes / sizeof kFpcrModes[0], t.bad);
        fail |= t.bad != 0;
        // microbenchmark on realistic matrices, default FPCR
        std::vector<float> mats(4096 * 16), svs(4096 * 3);
        for (int i = 0; i < 4096; i++) { MakeMatrix96d390(r, 2, &mats[16 * i]); makeScale(2); memcpy(&svs[3 * i], sv, 12); }
        alignas(16) static float outs[3][4];
        for (Cpu *c : {cg, cn}) { c->fpcr = 0; c->x[0] = (uint64_t)(uintptr_t)outs[0]; c->x[1] = (uint64_t)(uintptr_t)outs[1]; c->x[2] = (uint64_t)(uintptr_t)outs[2]; }
        rt_fpcr_sync(cg);
        auto args = [&](Cpu *c, int i) { c->x[3] = (uint64_t)(uintptr_t)&mats[16 * (i & 4095)]; c->x[4] = (uint64_t)(uintptr_t)&svs[3 * (i & 4095)]; };
        if (scaled)
            Bench("96d5f0", [&](int i) { args(cg, i); f_710096d5f0(cg); }, [&](int i) { args(cn, i); Native96d5f0(cn); }, 100000);
        else
            Bench("96d390", [&](int i) { args(cg, i); f_710096d390(cg); }, [&](int i) { args(cn, i); Native96d390(cn); }, 100000);
    }

    {   // main+0x155600
        Totals t;
        Rng r(0x155600);
        const long n = std::max(1L, cases / 10);
        for (long i = 0; i < n; i++) {
            const FdnInput f = MakeFdn(r, (int)(i % 4));
            for (uint64_t mode : kFpcrModes) Case155600(cg, cn, f, mode, r.g(), t);
        }
        long real = 0;
        for (const Capture &k : cap) {
            // record: u32 n, 0x90 bytes of state, n input floats, n output floats, then each line's buffer (len floats)
            if (k.tag != 0x155600 || k.bytes.size() < 4 + 0x90) continue;
            FdnInput f;
            const uint8_t *p = k.bytes.data();
            memcpy(&f.n, p, 4); memcpy(f.st, p + 4, 0x90);
            size_t at = 4 + 0x90, need = at + 8ull * f.n;
            uint32_t len[4];
            for (int i = 0; i < 4; i++) { memcpy(&len[i], f.st + 0x18 * i, 4); need += 4ull * len[i]; }
            if (need != k.bytes.size()) continue;
            auto take = [&](std::vector<float> &v, size_t cnt) { v.resize(cnt); memcpy(v.data(), p + at, 4 * cnt); at += 4 * cnt; };
            take(f.in, f.n); take(f.out, f.n);
            for (int i = 0; i < 4; i++) take(f.buf[i], len[i]);
            real++;
            for (uint64_t mode : kFpcrModes) Case155600(cg, cn, f, mode, real, t);
        }
        printf("155600 ReverbFdn4: %ld cases (%ld captured inputs x %zu FPCR modes), %ld mismatches\n", t.cases, real,
               sizeof kFpcrModes / sizeof kFpcrModes[0], t.bad);
        fail |= t.bad != 0;
        // microbenchmark: one realistic network, 1024-sample frames, default FPCR
        FdnInput f = MakeFdn(r, 3);
        static FdnInput sg, sn;
        sg = f; sn = f;
        for (Cpu *c : {cg, cn}) {
            FdnInput &s = c == cg ? sg : sn;
            for (int i = 0; i < 4; i++) { uint64_t p = (uint64_t)(uintptr_t)s.buf[i].data(); memcpy(s.st + 0x18 * i + 8, &p, 8); }
            c->fpcr = 0; c->x[0] = (uint64_t)(uintptr_t)s.st; c->x[1] = (uint64_t)(uintptr_t)s.in.data();
            c->x[3] = (uint64_t)(uintptr_t)s.out.data();
        }
        rt_fpcr_sync(cg);
        Bench("155600 (1024 samples)", [&](int) { cg->x[0] = (uint64_t)(uintptr_t)sg.st; cg->x[1] = (uint64_t)(uintptr_t)sg.in.data(); cg->x[2] = 1024; cg->x[3] = (uint64_t)(uintptr_t)sg.out.data(); f_7100155600(cg); },
              [&](int) { cn->x[0] = (uint64_t)(uintptr_t)sn.st; cn->x[1] = (uint64_t)(uintptr_t)sn.in.data(); cn->x[2] = 1024; cn->x[3] = (uint64_t)(uintptr_t)sn.out.data(); Native155600(cn); }, 200);
    }
    {   // main+0x8b5f10
        Totals t;
        Rng r(0x8b5f10);
        const long n = std::max(1L, cases / 10);
        for (long i = 0; i < n; i++) {
            const HierWorld w = MakeHier(r, (int)(i % 5));
            for (uint64_t mode : kFpcrModes) CaseHier(cg, cn, w, mode, t);
        }
        printf("8b5f10 HierarchyTransform: %ld cases (random skeletons x %zu FPCR modes), %ld mismatches\n", t.cases,
               sizeof kFpcrModes / sizeof kFpcrModes[0], t.bad);
        fail |= t.bad != 0;
        // microbenchmark: one 24-node skeleton of TRS matrices (the walk rewrites them each call: same work each time)
        HierWorld w;
        do { w = MakeHier(r, 2); } while (w.t.top < 3000);
        static std::vector<uint8_t> ag, an, ag0, an0;
        Place(w.t, ag); Place(w.t, an);
        ag0 = ag; an0 = an;
        for (Cpu *c : {cg, cn}) { c->fpcr = 0; c->sp = (uint64_t)(uintptr_t)(StackFor(c) + kStack - 0x800); }
        rt_fpcr_sync(cg);
        // each call starts from the same matrices (the walk rewrites them in place); both sides pay the copy
        auto setup = [&](Cpu *c, std::vector<uint8_t> &a) {
            memcpy(a.data(), (c == cg ? ag0 : an0).data(), w.t.top);
            c->x[0] = (uint64_t)(uintptr_t)a.data() + w.skel; c->x[1] = 0xffffffffu; c->x[2] = (uint64_t)(uintptr_t)a.data() + w.ctx;
        };
        Bench("8b5f10 (one skeleton)", [&](int) { setup(cg, ag); f_71008b5f10(cg); }, [&](int) { setup(cn, an); Native8b5f10(cn); }, 2000);
    }

    for (int loop = 0; loop < 2; loop++) {   // main+0x8b6750 bone loops
        Totals t;
        Rng r(0x8b6750 + loop);
        const long n = std::max(1L, cases / 2);
        for (long i = 0; i < n; i++) {
            const BoneWorld w = MakeBone(r, (int)(i % 5));
            for (uint64_t mode : kFpcrModes) CaseBone(cg, cn, w, loop, r, mode, t);
        }
        printf("8b6750 bone loop %d body: %ld cases (x %zu FPCR modes), %ld mismatches\n", loop + 1, t.cases,
               sizeof kFpcrModes / sizeof kFpcrModes[0], t.bad);
        fail |= t.bad != 0;
        // microbenchmark: TRS matrices, a parent, scaled flags as drawn
        const BoneWorld w = MakeBone(r, 2);
        static std::vector<uint8_t> ag, an;
        Place(w.t, ag); Place(w.t, an);
        for (Cpu *c : {cg, cn}) {
            const uint64_t base = (uint64_t)(uintptr_t)(c == cg ? ag : an).data();
            SetFrame(c, base + w.svs);
            c->fpcr = 0;
            c->x[8] = 1; c->x[11] = 1; c->x[9] = 0; c->x[10] = 0; c->x[14] = base + w.mats; c->x[15] = base + w.flags;
            c->x[16] = base + w.outT; c->x[25] = base + w.outQ; c->x[17] = base + w.outS; c->x[20] = base + w.root;
            if (loop) { c->x[0] = base + w.out2; c->x[9] = 1; c->x[10] = 0; c->x[11] = 0; c->x[24] = base + w.recs; c->x[27] = 0x60; c->x[8] = c->x[24] + 0x60; }
        }
        rt_fpcr_sync(cg);
        Cpu sg = *cg, sn = *cn;
        if (loop == 0)
            Bench("8b6750 loop 1 body", [&](int) { memcpy(cg->x, sg.x, sizeof sg.x); rg_8b7400(cg); }, [&](int) { memcpy(cn->x, sn.x, sizeof sn.x); NativeBone1(cn); }, 20000);
        else
            Bench("8b6750 loop 2 body", [&](int) { memcpy(cg->x, sg.x, sizeof sg.x); rg_8b7ac0(cg); }, [&](int) { memcpy(cn->x, sn.x, sizeof sn.x); NativeBone2(cn); }, 20000);
    }

#ifdef HOTFN_HOOKED
    printf("hooked: check-mode mismatches %ld\n", rt_hotfn_mismatches());
    fail |= rt_hotfn_mismatches() != 0;
#endif
    return fail;
}
