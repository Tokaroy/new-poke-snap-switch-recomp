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

// ---------------------------------------------------------------- 4x4 row matrices (bone and node transforms)

#define HF_S __builtin_shufflevector

// m[0]*a0 + m[1]*a1 + m[2]*a2 + m[3]*a3 as the guest's FMUL-by-element and three FMLA-by-element (row m times the
// matrix whose rows are a0..a3)
inline vf32 MulRow(Cpu *c, vf32 a0, vf32 a1, vf32 a2, vf32 a3, vf32 m) {
    return Fma(c, Fma(c, Fma(c, Mul(c, a0, Splat(m[0])), a1, Splat(m[1])), a2, Splat(m[2])), a3, Splat(m[3]));
}

// Inverse of the 4x4 row matrix p0..p3 the way the game's inlined vector-math inverse computes it (the same sequence at
// main+0x8b5f10 and in both bone loops of main+0x8b6750): 2x2 cofactor products of the transposed matrix, sign vectors
// sa = (1,-1,1,-1) and sb = (-1,1,-1,1) (the guest reads them from its stack copy), determinant by FADD/FADDP, 1/det by
// FRECPE and two FRECPS steps, and every row zeroed when the determinant is 0. Each line is one guest instruction, in
// an order that keeps each FP operation's operands as the guest has them.
inline void Inverse4(Cpu *c, vf32 p0, vf32 p1, vf32 p2, vf32 p3, vf32 sa, vf32 sb, vf32 inv[4]) {
    const vf32 t1 = Mul(c, HF_S(p2, p3, 6, 6, 6, 2), HF_S(p1, p2, 7, 7, 3, 3));
    const vf32 t2 = Mul(c, HF_S(p1, p2, 6, 6, 2, 2), HF_S(p2, p3, 7, 7, 7, 3));
    const vf32 t3 = Mul(c, HF_S(p0, p1, 7, 3, 3, 3), HF_S(p2, p3, 6, 6, 6, 2));
    const vf32 t4 = Mul(c, HF_S(p0, p1, 7, 3, 3, 3), HF_S(p1, p2, 6, 6, 2, 2));
    const vf32 t5 = Sub(c, t2, t1);
    const vf32 t7 = Sub(c, Mul(c, HF_S(p0, p1, 6, 2, 2, 2), HF_S(p2, p3, 7, 7, 7, 3)), t3);
    const vf32 t9 = Sub(c, Mul(c, HF_S(p0, p1, 6, 2, 2, 2), HF_S(p1, p2, 7, 7, 3, 3)), t4);
    const vf32 t11 = Fma(c, Mul(c, HF_S(p0, p1, 5, 1, 1, 1), t5), vneg_s(HF_S(p1, p2, 5, 5, 1, 1)), t7);
    const vf32 t13 = Fma(c, Mul(c, HF_S(p0, p1, 4, 0, 0, 0), t5), vneg_s(HF_S(p1, p2, 4, 4, 0, 0)), t7);
    const vf32 t14 = Mul(c, HF_S(p1, p2, 4, 4, 0, 0), HF_S(p2, p3, 5, 5, 5, 1));
    const vf32 t15 = Fma(c, t11, t9, HF_S(p2, p3, 5, 5, 5, 1));
    const vf32 t16 = Fma(c, t13, t9, HF_S(p2, p3, 4, 4, 4, 0));
    const vf32 t17 = Mul(c, HF_S(p2, p3, 4, 4, 4, 0), HF_S(p1, p2, 5, 5, 1, 1));
    const vf32 t18 = Mul(c, HF_S(p2, p3, 4, 4, 4, 0), HF_S(p0, p1, 5, 1, 1, 1));
    const vf32 t19 = Sub(c, t14, t17);
    const vf32 t20 = Mul(c, HF_S(p0, p1, 4, 0, 0, 0), HF_S(p2, p3, 5, 5, 5, 1));
    const vf32 t21 = Mul(c, HF_S(p0, p1, 4, 0, 0, 0), HF_S(p1, p2, 5, 5, 1, 1));
    const vf32 t22 = Mul(c, HF_S(p0, p1, 5, 1, 1, 1), HF_S(p1, p2, 4, 4, 0, 0));
    const vf32 t23 = Sub(c, t20, t18);
    const vf32 t24 = Mul(c, t15, sa);
    const vf32 t25 = Sub(c, t21, t22);
    const vf32 t27 = Fma(c, Mul(c, HF_S(p0, p1, 7, 3, 3, 3), t19), vneg_s(HF_S(p1, p2, 7, 7, 3, 3)), t23);
    const vf32 t29 = Fma(c, Mul(c, HF_S(p0, p1, 6, 2, 2, 2), t19), vneg_s(HF_S(p1, p2, 6, 6, 2, 2)), t23);
    const vf32 t30 = Mul(c, (vf32){p0[0], p1[0], p2[0], p3[0]}, t24);   // column 0 times the first cofactor row
    const vf32 t31 = Mul(c, t16, sb);
    const vf32 t32 = Add(c, HF_S(t30, HF_Z, 2, 3, 4, 4), HF_S(t30, HF_Z, 0, 1, 4, 4));   // FADD 2S of the halves
    const vf32 t33 = Add(c, HF_S(t32, HF_Z, 0, 0, 4, 4), HF_S(t32, HF_Z, 1, 1, 4, 4));   // FADDP 2S: det in lanes 0, 1
    const vf32 det = HF_S(t33, t33, 0, 1, 0, 1);
    const vf32 t34 = Fma(c, t29, t25, HF_S(p2, p3, 6, 6, 6, 2));
    const vf32 t35 = vrecpe_s(c, det);
    const vf32 t36 = Fma(c, t27, t25, HF_S(p2, p3, 7, 7, 7, 3));
    const vf32 t37 = Mul(c, t34, sb);
    const vf32 t38 = vop_s(c, FOP_RECPS, vneg_s(t35), det, HF_Z);
    const vf32 t39 = Mul(c, t36, sa);
    const vf32 t40 = Mul(c, t35, t38);
    const vf32 t41 = vop_s(c, FOP_RECPS, vneg_s(t40), det, HF_Z);
    const vu32 nz = ~vcmp_s(c, 0, det, HF_Z);   // FCMEQ #0 then MVN
    const vf32 r = Mul(c, t40, t41);              // 1 / det
    inv[0] = (vf32)((vu32)Mul(c, r, t24) & nz);
    inv[1] = (vf32)((vu32)Mul(c, r, t31) & nz);
    inv[2] = (vf32)((vu32)Mul(c, t39, r) & nz);
    inv[3] = (vf32)((vu32)Mul(c, t37, r) & nz);
}

// Guest memory access for the hierarchy walk: direct, or (check mode) with the walk's stores kept aside so the guest
// can run afterwards on unchanged memory. Shadow::bad is set when a load partly overlaps a kept store.
struct DirectMem {
    vf32 Ld4(uint64_t a) { return hotfn::Ld4(a); }
    void St4(uint64_t a, vf32 v) { hotfn::St4(a, v); }
    uint32_t Ld32(uint64_t a) { return hotfn::Ld32(a); }
    uint64_t Ld64(uint64_t a) { return hotfn::Ld64(a); }
    uint8_t Ld8(uint64_t a) { return *(const uint8_t *)(uintptr_t)a; }
};

// main+0x8b5f10 (recursive): for each child of node w1 (w1 = -1: the root list) of the skeleton x0, except the node
// ctx+0x10 names, find its 4x4 matrix D (index tables and the flag at +0x999 pick the address, as the guest does) and
// set rows 0-2 to D[i] times (A0, A1, A2, (0,0,0,1)) with A = *(ctx+0x18), and row 3 to (D[3] times inverse(*(ctx+0x28)))
// with w = 0 times *(ctx+0x20) plus its row 3 (FMLA by a vector of ones); then the same for that child's children.
template <class Mem> void HierarchyTransform(Cpu *c, Mem &mem, uint64_t x0, uint32_t w1, uint64_t ctx) {
    const uint64_t p = mem.Ld64(x0);
    uint64_t node;
    if (w1 != 0xffffffffu) {
        const uint64_t e = mem.Ld64(mem.Ld64(p + 0x48) + (uint64_t)(int64_t)(int32_t)w1 * 8);
        if (!e) return;
        node = mem.Ld64(e + 8);
    } else {
        node = mem.Ld64(mem.Ld64(p + 0xb0) + 8);
    }
    const vf32 kW = {0.0f, 0.0f, 0.0f, 1.0f}, kOne = Splat(1.0f);
    const vf32 kSa = {1.0f, -1.0f, 1.0f, -1.0f}, kSb = {-1.0f, 1.0f, -1.0f, 1.0f};
    for (; node; node = mem.Ld64(node + 0x20)) {
        const uint32_t id = mem.Ld32(node + 0x30);
        if (mem.Ld32(ctx + 0x10) == id) continue;
        const uint64_t x10 = mem.Ld64(ctx), x11 = mem.Ld64(ctx + 8);
        uint32_t idx = 0xffffffffu;
        if (mem.Ld64(x11 + 0x48) > (uint64_t)id) idx = mem.Ld32(mem.Ld64(x11 + 0x40) + (uint64_t)id * 4);
        uint64_t d = 0;
        const uint64_t o = mem.Ld64(x10), q = mem.Ld64(o + 0x18);
        if (q && mem.Ld64(q) == mem.Ld64(o + 0x20)) {
            const uint64_t x12 = mem.Ld64(o + 0x10);
            if (mem.Ld64(x12 + 0x48) > (uint64_t)idx) {
                const uint64_t x13 = mem.Ld64(o + 0x30);
                if (mem.Ld64(x13 + 0x20)) {
                    const uint64_t x11b = mem.Ld64(x13 + 0x18);
                    if (mem.Ld8(x12 + 0x999) == 0)
                        d = mem.Ld64(o) + (uint64_t)mem.Ld32(x13 + 0x10) * idx + mem.Ld32(x11b);
                    else
                        d = mem.Ld64(o) + (uint32_t)(mem.Ld32(x11b + 0x34) * idx + mem.Ld32(mem.Ld64(x12 + 0x858)));
                }
            }
        }
        const uint64_t ma = mem.Ld64(ctx + 0x18), mb = mem.Ld64(ctx + 0x20), mc = mem.Ld64(ctx + 0x28);
        const vf32 c0 = mem.Ld4(mc), c1 = mem.Ld4(mc + 16), c2 = mem.Ld4(mc + 32), c3 = mem.Ld4(mc + 48);
        const vf32 d0 = mem.Ld4(d), d1 = mem.Ld4(d + 16), d2 = mem.Ld4(d + 32), d3 = mem.Ld4(d + 48);
        const vf32 a0 = mem.Ld4(ma), a1 = mem.Ld4(ma + 16), a2 = mem.Ld4(ma + 32);
        const vf32 b0 = mem.Ld4(mb), b1 = mem.Ld4(mb + 16), b2 = mem.Ld4(mb + 32), b3 = mem.Ld4(mb + 48);
        vf32 inv[4];
        Inverse4(c, c0, c1, c2, c3, kSa, kSb, inv);
        const vf32 r0 = MulRow(c, a0, a1, a2, kW, d0);
        const vf32 r1 = MulRow(c, a0, a1, a2, kW, d1);
        const vf32 r2 = MulRow(c, a0, a1, a2, kW, d2);
        const vf32 t = MulRow(c, inv[0], inv[1], inv[2], inv[3], d3);
        const vf32 r3 = Fma(c, Fma(c, Fma(c, Mul(c, b0, Splat(t[0])), b1, Splat(t[1])), b2, Splat(t[2])), kOne, b3);
        mem.St4(d, r0); mem.St4(d + 16, r1); mem.St4(d + 32, r2); mem.St4(d + 48, r3);
        HierarchyTransform(c, mem, x0, mem.Ld32(node + 0x30), ctx);
    }
}

// The per-bone body of the two local-pose loops of main+0x8b6750 (bone update): the bone's matrix relative to its
// parent, then the decomposition call. Computed from the guest registers at the loop body; the caller stores the
// local matrix where the guest does and runs MatrixDecompose / ScaledMatrixDecompose on it.
struct BoneLocal {
    uint64_t outS, outQ, outT;   // decomposition outputs (x0, x1, x2)
    uint64_t mat;                // where the guest keeps the local matrix (x3)
    uint64_t sv;                 // scaled: the scale vector (x4); 0: plain decomposition
    vf32 m[4];
    vf32 scale;                  // loop 1 scaled: the 12 bytes the guest copies to x4 first
    bool copyScale;
};

// rows of child matrix ch times the matrix whose rows are a[0..3]
inline void MulMat(Cpu *c, const vf32 a[4], uint64_t ch, vf32 out[4]) {
    for (int i = 0; i < 4; i++) out[i] = MulRow(c, a[0], a[1], a[2], a[3], Ld4(ch + 16 * i));
}

// loop 1, at main+0x8b7400 (after the id checks): w8 child id, w11 its matrix index, w9 parent id (-1: root), w10 the
// parent's matrix index, x14 matrices (64 bytes each), x15 per-index "scaled" flags or 0, x16/x25/x17 the translation /
// rotation / scale arrays (16 bytes per id), x20+0x10 the root matrix, [sp+8] per-index scale vectors, sign vectors at
// sp+0x20 / sp+0x10; the local matrix goes to x29-0xe0, the scale copy to x29-0x98.
inline BoneLocal BoneLoop1(Cpu *c) {
    BoneLocal b{};
    const uint64_t ci = (uint32_t)c->x[8] * 16ull, cm = (uint32_t)c->x[11];
    b.outS = c->x[17] + ci; b.outQ = c->x[25] + ci; b.outT = c->x[16] + ci;
    b.mat = c->x[29] - 0xe0;
    const uint64_t ch = c->x[14] + cm * 64;
    vf32 a[4];
    if ((uint32_t)c->x[9] == 0xffffffffu) {
        for (int i = 0; i < 4; i++) a[i] = Ld4(c->x[20] + 0x10 + 16 * i);
        MulMat(c, a, ch, b.m);
        return b;
    }
    const uint64_t pi = (uint32_t)c->x[10], pm = c->x[14] + pi * 64;
    Inverse4(c, Ld4(pm), Ld4(pm + 16), Ld4(pm + 32), Ld4(pm + 48), Ld4(c->sp + 0x20), Ld4(c->sp + 0x10), a);
    MulMat(c, a, ch, b.m);
    if (c->x[15] && Ld32(c->x[15] + cm * 4)) {
        b.sv = c->x[29] - 0x98;
        b.scale = Ld4(Ld64(c->sp + 8) + pi * 16);
        b.copyScale = true;
    }
    return b;
}

// loop 2, at main+0x8b7ac0: w9 child index, w10 parent id (-1: root), w11 parent index, x0 the bone's output (scale,
// +0x10 rotation, +0x20 translation), records of x27 (0x60) bytes at x24 (matrix at +0, "scaled" flag at +0x50, scale
// vector at +0x40), sign vectors at sp+0x50 / sp+0x40; the local matrix goes to sp+0x60. Returns false for the path the
// guest takes when the parent record address is 0 (no decomposition; left to the guest).
inline bool BoneLoop2(Cpu *c, BoneLocal &b) {
    b = BoneLocal{};
    const uint64_t x0 = c->x[0], x24 = c->x[24], x27 = c->x[27];
    const uint64_t ch = (uint32_t)c->x[9] * x27 + x24;
    b.outS = x0; b.outQ = x0 + 0x10; b.outT = x0 + 0x20;
    b.mat = c->sp + 0x60;
    vf32 a[4];
    if ((uint32_t)c->x[10] == 0xffffffffu) {
        for (int i = 0; i < 4; i++) a[i] = Ld4(c->x[20] + 0x10 + 16 * i);
        MulMat(c, a, ch, b.m);
        return true;
    }
    const uint64_t pm = (uint64_t)(uint32_t)c->x[11] * (uint32_t)x27 + x24;   // UMADDL
    if (!pm) return false;
    Inverse4(c, Ld4(pm), Ld4(pm + 16), Ld4(pm + 32), Ld4(pm + 48), Ld4(c->sp + 0x50), Ld4(c->sp + 0x40), a);
    MulMat(c, a, ch, b.m);
    if (Ld32(ch + 0x50)) b.sv = pm + 0x40;
    return true;
}

// stores the local matrix (and loop 1's scale copy) at the given places and decomposes it
inline void BoneFinish(Cpu *c, const BoneLocal &b, uint64_t mat, uint64_t sv, uint64_t outS, uint64_t outQ, uint64_t outT, uint64_t epsSlot) {
    for (int i = 0; i < 4; i++) St4(mat + 16 * i, b.m[i]);
    if (b.copyScale) { memcpy((void *)(uintptr_t)sv, &b.scale, 8); memcpy((void *)(uintptr_t)(sv + 8), (const char *)&b.scale + 8, 4); }
    if (b.sv) ScaledMatrixDecompose(c, outS, outQ, outT, mat, sv, epsSlot);
    else MatrixDecompose(c, outS, outQ, outT, mat, epsSlot);
}

}   // namespace hotfn
