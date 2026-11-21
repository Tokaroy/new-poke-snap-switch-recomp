// Native versions of hot leaf routines of the game's main module (decomp-for-speed pass, 2026-11-20; project code, ours).
//
// Each function here does what one guest function does, with the guest's own operation order, so the results are
// bit-exact: every FP operation goes through the same runtime helper the generated code uses (runtime/simd.h vop_s,
// op_s, cmp_s, rsqrte_s: ARM NaN rules, FPCR.FZ / DN / RMode honoured), only the values live in host registers instead
// of the Cpu register file, and table lookups (TBL) and lane moves become fixed shuffles. Shared by the hooks in
// host/mods/hotfn.cpp and the differential test tests/hotfn/hotfn_test.cpp (which runs the generated guest function
// next to these on random and real inputs). Guest memory is host memory (guest address == host address).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

#include "cpu.h"
#include "simd.h"

namespace hotfn {

inline vf32 Ld4(uint64_t a) { vf32 v; memcpy(&v, (const void *)(uintptr_t)a, 16); return v; }
inline void St4(uint64_t a, vf32 v) { memcpy((void *)(uintptr_t)a, &v, 16); }
inline float Ldf(uint64_t a) { float v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
inline void Stf(uint64_t a, float v) { memcpy((void *)(uintptr_t)a, &v, 4); }
inline uint32_t Ld32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
inline void St32(uint64_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }
inline uint64_t Ld64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }

#define HF_Z ((vf32){})
inline vf32 Add(Cpu *c, vf32 a, vf32 b) { return vop_s(c, FOP_ADD, a, b, HF_Z); }
inline vf32 Sub(Cpu *c, vf32 a, vf32 b) { return vop_s(c, FOP_SUB, a, b, HF_Z); }
inline vf32 Mul(Cpu *c, vf32 a, vf32 b) { return vop_s(c, FOP_MUL, a, b, HF_Z); }
inline vf32 Fma(Cpu *c, vf32 acc, vf32 a, vf32 b) { return vop_s(c, FOP_MADD, acc, a, b); }   // FMLA acc += a * b (fused)
inline vf32 Lo(vf32 v) { return (vf32){v[0], v[1], 0, 0}; }                                   // a 2S (Q = 0) operand
inline vf32 Splat(float x) { return (vf32){x, x, x, x}; }
inline vf32 Sel(vu32 m, vf32 a, vf32 b) { return (vf32)(((vu32)a & m) | ((vu32)b & ~m)); }   // BSL: m ? a : b

// Sum of squares the way the guest reduces a 4-lane vector: FMUL v,v; EXT #8; FADD 2S; FADDP 2S; DUP (lanes 0..3 all
// equal to (s2 + s0) + (s3 + s1)).
inline vf32 SumSq(Cpu *c, vf32 v) {
    const vf32 sq = Mul(c, v, v);
    const vf32 p = Add(c, Lo(SHUF(sq, sq, 2, 3, 0, 1)), Lo(sq));
    const vf32 s = Lo(Add(c, SHUF(p, p, 0, 4, 2, 2), SHUF(p, p, 1, 5, 2, 2)));
    return Splat(s[0]);
}

// One Newton step pair as the guest writes it: r = FRSQRTE(n); r *= FRSQRTS(r, n * r) twice. n has four equal lanes
// (SumSq), so the table estimate is computed once and splat (the guest computes the same value in each lane).
inline vf32 Rsqrt2(Cpu *c, vf32 n) {
    vf32 r = Splat(rsqrte_s(c, n[0]));
    vf32 t = Mul(c, r, n);
    t = vop_s(c, FOP_RSQRTS, vneg_s(r), t, HF_Z);
    r = Mul(c, r, t);
    t = Mul(c, n, r);
    t = vop_s(c, FOP_RSQRTS, vneg_s(r), t, HF_Z);
    return Mul(c, r, t);
}

// Rotation quaternion of the normalized rows r0..r2 (largest-diagonal pick), shared tail of main+0x96d390 and
// main+0x96d5f0: normalized with FRSQRTE + two Newton steps (zero when its length is 0), then normalized again and
// zeroed when its squared length is under nn::util::detail::FloatQuaternionEpsilon (*(float **)epsSlot).
inline vf32 QuatFromRows(Cpu *c, vf32 r0, vf32 r1, vf32 r2, uint64_t epsSlot) {
    // trace terms T = (1 + x - y - z, 1 - x + y - z, 1 - x - y + z, 1 + x + y + z), x/y/z = diagonal
    vf32 t = Mul(c, (vf32){1, -1, -1, 1}, Splat(r0[0]));
    t = Fma(c, t, (vf32){-1, 1, -1, 1}, Splat(r1[1]));
    t = Fma(c, t, (vf32){-1, -1, 1, 1}, Splat(r2[2]));
    t = Add(c, t, Splat(1.0f));
    // off-diagonal sums A and differences D (lane 3 is 0 + 0)
    const vf32 u = {r0[1], r2[0], r1[2], 0}, w = {r1[0], r0[2], r2[1], 0};
    const vf32 a = Add(c, u, w), d = Sub(c, u, w);
    const vf32 q0 = {t[0], a[0], a[1], d[2]};
    const vf32 q1 = {a[0], t[1], a[2], d[1]};
    const vf32 q2 = {a[1], a[2], t[2], d[0]};
    const vf32 q3 = {d[2], d[1], d[0], t[3]};
    const vf32 t0 = Splat(t[0]), t1 = Splat(t[1]), t2 = Splat(t[2]), t3 = Splat(t[3]);
    vu32 k = vcmp_s(c, 1, t1, t0);                       // FCMGE
    vf32 q = Sel(k, q1, q0), best = Sel(k, t1, t0);
    k = vcmp_s(c, 1, t2, best);
    q = Sel(k, q2, q);
    best = Sel(k, t2, best);
    k = vcmp_s(c, 1, t3, best);
    q = Sel(k, q3, q);

    vf32 n = SumSq(c, q);
    vu32 nz = ~vcmp_s(c, 0, n, HF_Z);                    // FCMEQ #0, inverted
    q = (vf32)((vu32)Mul(c, Rsqrt2(c, n), q) & nz);
    const vf32 eps = Splat(Ldf(Ld64(epsSlot)));
    n = SumSq(c, q);
    const vu32 tiny = vcmp_s(c, 2, eps, n);              // FCMGT eps > n
    q = (vf32)((vu32)Mul(c, q, Rsqrt2(c, n)) & ~tiny);
    return q;
}

// main+0x96d390: decompose a 4x4 row matrix (x3; rows of 4 floats, row 3 = translation) into scale (x0, 3 floats),
// rotation quaternion (x1, 4 floats) and translation (x2, 3 floats); the SDK vector-math inline
// (nn::util MatrixDecompose style). Scale = length of each row (all four lanes); a positive length normalizes its row;
// the quaternion comes from the normalized 3x3 by the largest-diagonal pick, is normalized with FRSQRTE + two Newton
// steps (zero when its length is 0), and normalized again, zeroed when its squared length is under
// nn::util::detail::FloatQuaternionEpsilon (read through the GOT slot main+0xccb210, as the guest does).
inline void MatrixDecompose(Cpu *c, uint64_t outScale, uint64_t outQuat, uint64_t outTrans, uint64_t m, uint64_t epsSlot) {
    vf32 r0 = Ld4(m), r1 = Ld4(m + 16), r2 = Ld4(m + 32);
    const vf32 r3 = Ld4(m + 48);
    const vf32 l0 = vop_s(c, FOP_SQRT, SumSq(c, r0), SumSq(c, r0), HF_Z);
    const vf32 l1 = vop_s(c, FOP_SQRT, SumSq(c, r1), SumSq(c, r1), HF_Z);
    // the translation is stored before the third length's square root (no difference unless the outputs alias the input)
    memcpy((void *)(uintptr_t)outTrans, &r3, 8);
    Stf(outTrans + 8, r3[2]);
    const vf32 l2 = vop_s(c, FOP_SQRT, SumSq(c, r2), SumSq(c, r2), HF_Z);
    const float sc[2] = {l0[0], l1[0]};
    memcpy((void *)(uintptr_t)outScale, sc, 8);
    Stf(outScale + 8, l2[0]);
    if (cmp_s(c, l0[0], 0.0f) == 0x20000000ull) r0 = Mul(c, r0, Splat(op_s(c, FOP_DIV, 1.0f, l0[0], 0.0f)));
    if (cmp_s(c, l1[0], 0.0f) == 0x20000000ull) r1 = Mul(c, r1, Splat(op_s(c, FOP_DIV, 1.0f, l1[0], 0.0f)));
    if (cmp_s(c, l2[0], 0.0f) == 0x20000000ull) r2 = Mul(c, r2, Splat(op_s(c, FOP_DIV, 1.0f, l2[0], 0.0f)));

    St4(outQuat, QuatFromRows(c, r0, r1, r2, epsSlot));
}

// Row r scaled by 1/l the way main+0x96d5f0 does it: FRECPE of (l, l, l, 1) and two FRECPS Newton steps.
inline vf32 RecipScale(Cpu *c, vf32 r, float l) {
    const vf32 v = {l, l, l, 1.0f};
    const float e = recpe_s(c, l);
    vf32 x = {e, e, e, recpe_s(c, 1.0f)};
    x = Mul(c, x, vop_s(c, FOP_RECPS, vneg_s(x), v, HF_Z));
    const vf32 y = Mul(c, vop_s(c, FOP_RECPS, vneg_s(x), v, HF_Z), x);
    return Mul(c, r, y);
}

// main+0x96d5f0: the same decomposition for the matrix x3 with its 3x3 part first scaled per column by the vector x4
// (3 floats): row i becomes (s.x*m[i].x, s.y*m[i].y, s.z*m[i].z, m[i].w), each as the guest's masked FMUL/FMLA chain.
// Outputs as in MatrixDecompose; a row is normalized whenever its length is not exactly zero (NaN included), by the
// reciprocal estimate path above instead of a division.
inline void ScaledMatrixDecompose(Cpu *c, uint64_t outScale, uint64_t outQuat, uint64_t outTrans, uint64_t m, uint64_t sv,
                                  uint64_t epsSlot) {
    const vf32 m0 = Ld4(m), m1 = Ld4(m + 16), m2 = Ld4(m + 32), m3 = Ld4(m + 48);
    memcpy((void *)(uintptr_t)outTrans, &m3, 8);
    Stf(outTrans + 8, m3[2]);
    const vf32 sx = {Ldf(sv), 0, 0, 0}, sy = {0, Ldf(sv + 4), 0, 0}, sz = {0, 0, Ldf(sv + 8), 0}, w = {0, 0, 0, 1.0f};
    auto row = [&](vf32 r) {
        vf32 o = Mul(c, sx, Splat(r[0]));
        o = Fma(c, o, sy, Splat(r[1]));
        o = Fma(c, o, sz, Splat(r[2]));
        return Fma(c, o, w, Splat(r[3]));
    };
    vf32 r0 = row(m0), r1 = row(m1), r2 = row(m2);
    const float l0 = vop_s(c, FOP_SQRT, SumSq(c, r0), SumSq(c, r0), HF_Z)[0];
    const float l1 = vop_s(c, FOP_SQRT, SumSq(c, r1), SumSq(c, r1), HF_Z)[0];
    const float l2 = vop_s(c, FOP_SQRT, SumSq(c, r2), SumSq(c, r2), HF_Z)[0];
    Stf(outScale + 8, l2);
    const float sc[2] = {l0, l1};
    memcpy((void *)(uintptr_t)outScale, sc, 8);
    if (cmp_s(c, l0, 0.0f) != 0x60000000ull) r0 = RecipScale(c, r0, l0);   // B.EQ skips on an exact zero only
    if (cmp_s(c, l1, 0.0f) != 0x60000000ull) r1 = RecipScale(c, r1, l1);
    if (cmp_s(c, l2, 0.0f) != 0x60000000ull) r2 = RecipScale(c, r2, l2);
    St4(outQuat, QuatFromRows(c, r0, r1, r2, epsSlot));
}

// main+0x155600: one channel of the Wwise reverb's 4-line feedback delay network (audio render thread, AK::EventManager).
// x0 = state: four delay lines at +0x00/+0x18/+0x30/+0x48, each {u32 length, pad, float *buffer, u32 position}; then at
// +0x60 + 12*i per line {float filter state, float b, float a}. x1 = input samples (read), x3 = output samples (each
// output is accumulated into), w2 = sample count. Per line, a one-pole lowpass y = b*tap - a*y' of the delayed sample,
// then output += y0 - y1 + y2 - y3 and line i is fed input - 0.5*(y0+y1+y2+y3) + y(i+1 mod 4) (a Householder mix).
// The guest works in passes up to the nearest delay-line wrap; inside a pass it runs blocks of 4 samples (transposed,
// fused) and then single samples, which round differently, so this follows the same pass/block/tail split exactly.
inline void ReverbFdn4(Cpu *c, uint64_t st, uint64_t in, uint32_t n, uint64_t out) {
    vf32 y = {Ldf(st + 0x60), Ldf(st + 0x6c), Ldf(st + 0x78), Ldf(st + 0x84)};
    const vf32 b = {Ldf(st + 0x64), Ldf(st + 0x70), Ldf(st + 0x7c), Ldf(st + 0x88)};
    const vf32 na = vneg_s((vf32){Ldf(st + 0x68), Ldf(st + 0x74), Ldf(st + 0x80), Ldf(st + 0x8c)});
    const vf32 half = Splat(-0.5f);
    uint32_t len[4], pos[4];
    uint64_t buf[4];
    for (int i = 0; i < 4; i++) {
        len[i] = Ld32(st + 0x18 * i);
        buf[i] = Ld64(st + 0x18 * i + 8);
        pos[i] = Ld32(st + 0x18 * i + 0x10);
    }
    auto tap = [&](int i) { return buf[i] + 4 * (uint64_t)pos[i]; };
    do {
        uint32_t m = n;
        for (int i = 0; i < 4; i++) m = std::min(m, len[i] - pos[i]);
        for (uint32_t k = m >> 2; k; k--) {
            const vf32 l0 = Ld4(tap(0)), l1 = Ld4(tap(1)), l2 = Ld4(tap(2)), l3 = Ld4(tap(3));
            // sample j of every line: x_j = {l0[j], l1[j], l2[j], l3[j]}
            const vf32 x0 = {l0[0], l1[0], l2[0], l3[0]}, x1 = {l0[1], l1[1], l2[1], l3[1]};
            const vf32 x2 = {l0[2], l1[2], l2[2], l3[2]}, x3 = {l0[3], l1[3], l2[3], l3[3]};
            const vf32 y0 = Fma(c, Mul(c, y, na), x0, b);
            const vf32 y1 = Fma(c, Mul(c, x1, b), y0, na);
            const vf32 y2 = Fma(c, Mul(c, x2, b), y1, na);
            y = Fma(c, Mul(c, x3, b), y2, na);
            // line i over the four samples: L_i = {y0[i], y1[i], y2[i], y3[i]}
            const vf32 L0 = {y0[0], y1[0], y2[0], y[0]}, L1 = {y0[1], y1[1], y2[1], y[1]};
            const vf32 L2 = {y0[2], y1[2], y2[2], y[2]}, L3 = {y0[3], y1[3], y2[3], y[3]};
            const vf32 e = Add(c, L2, L0);
            St4(out, Sub(c, Sub(c, Add(c, e, Ld4(out)), L1), L3));
            const vf32 z = Fma(c, Ld4(in), Add(c, e, Add(c, L1, L3)), half);
            St4(tap(0), Add(c, z, L1)); pos[0] += 4;
            St4(tap(1), Add(c, z, L2)); pos[1] += 4;
            St4(tap(2), Add(c, z, L3)); pos[2] += 4;
            St4(tap(3), Add(c, z, L0)); pos[3] += 4;
            in += 16; out += 16;
        }
        for (uint32_t k = m & 3; k; k--) {
            const vf32 x = {Ldf(tap(0)), Ldf(tap(1)), Ldf(tap(2)), Ldf(tap(3))};
            y = Fma(c, Mul(c, y, na), x, b);
            const float p0 = op_s(c, FOP_ADD, y[2], y[0], 0.0f), p1 = op_s(c, FOP_ADD, y[3], y[1], 0.0f);
            Stf(out, op_s(c, FOP_ADD, op_s(c, FOP_SUB, Ldf(out), p1, 0.0f), p0, 0.0f));
            const float h = op_s(c, FOP_MUL, op_s(c, FOP_ADD, p0, p1, 0.0f), -0.5f, 0.0f);
            const vf32 w = Add(c, Splat(h), y);
            const vf32 v = Add(c, (vf32){w[1], w[2], w[3], w[0]}, Splat(Ldf(in)));
            for (int i = 0; i < 4; i++) { Stf(tap(i), v[i]); pos[i]++; }
            in += 4; out += 4;
        }
        for (int i = 0; i < 4; i++) if (pos[i] == len[i]) pos[i] = 0;
        n -= m;
    } while (n);
    for (int i = 0; i < 4; i++) {
        St32(st + 0x18 * i + 0x10, pos[i]);
        Stf(st + 0x60 + 12 * i, y[i]);
    }
}

}   // namespace hotfn
