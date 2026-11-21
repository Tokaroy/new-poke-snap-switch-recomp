// Host versions of the hot inner routines of the game's statically linked Kraken decoder (main+0x7240 OodleLZ_Decompress
// -> ... ; project code, ours). Derived from the guest machine code of these three functions, not from any Oodle SDK or
// other decoder source; tests/oodle compares them with the generated guest code on real compressed blocks.
//
//   main+0x4d220  HuffCore6   6-stream Huffman fast loop (two output halves x three bit streams: forward, backward,
//                             forward-from-middle; 11-bit LSB-first lookup table of uint16 len | sym << 8)
//   main+0x16680  LzRuns raw  Kraken LZ command loop, literals copied
//   main+0x16ed0  LzRuns delta the same, literals added to the byte at the last match offset
//
// HuffCore6 is a line-by-line transcription: same loads, same stores (including the one stale byte each 4-byte store
// leaves after the 15 decoded ones), same state written back. LzRuns is not a transcription: the guest copies in 8- and
// 16-byte words that run past the end of a literal run or match, but every such byte is overwritten later and the guest
// never writes outside [dst, dst_end) except 64 bytes of padding after the length stream, so a decoder that writes each
// output byte once leaves the same memory. That holds when every match offset is -8 or less (an 8-byte word copy then
// equals a byte copy). LzHeader rejects (run the guest) what it cannot check up front (stream overlaps, the padding);
// LzRun then decodes in place in one pass with the guest's own checks inline and stops (run the guest, which rewrites
// the whole chunk from the same inputs) on an offset above -8 or anything else the guest would reject. The check mode
// walks the commands first (LzReach) and decodes into a shadow copy instead.
#pragma once
#include <cstdint>
#include <cstring>

namespace hotfn {

// ---- main+0x4d220. st = the decoder's state block (0x90 bytes), lut = 2048 uint16 entries.
// Fields read: [0x00] out1, [0x08] out2, [0x18] out2 end, [0x30] [0x38] [0x40] [0x48] [0x50] [0x58] stream pointers
// (fwd, bwd, fwd, fwd, bwd, fwd). Written as the guest does: [0x00] [0x08] and the six pointers when the loop ran,
// [0x60 + 4 i] / [0x78 + 4 i] (bits left over, count) only for streams that stopped inside a byte.

inline uint64_t HLd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
inline void HSt32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
inline uint64_t SLd(const uint8_t *st, int o) { uint64_t v; memcpy(&v, st + o, 8); return v; }
inline void SSt(uint8_t *st, int o, uint64_t v) { memcpy(st + o, &v, 8); }
inline void SSt32(uint8_t *st, int o, uint32_t v) { memcpy(st + o, &v, 4); }

// returns the guest's x0 (1 = ok, 0 = the streams crossed)
inline uint64_t HuffCore6(uint8_t *st, const uint16_t *lut) {
    const uint8_t *s0 = (const uint8_t *)(uintptr_t)SLd(st, 0x30);   // x2  forward
    const uint8_t *s1 = (const uint8_t *)(uintptr_t)SLd(st, 0x38);   // x5  backward
    const uint8_t *s2 = (const uint8_t *)(uintptr_t)SLd(st, 0x40);   // x8  forward from the middle
    const uint8_t *s3 = (const uint8_t *)(uintptr_t)SLd(st, 0x48);   // x11
    const uint8_t *s4 = (const uint8_t *)(uintptr_t)SLd(st, 0x50);   // x14 backward
    const uint8_t *s5 = (const uint8_t *)(uintptr_t)SLd(st, 0x58);   // x17
    uint8_t *o1 = (uint8_t *)(uintptr_t)SLd(st, 0x00);
    uint8_t *o2 = (uint8_t *)(uintptr_t)SLd(st, 0x08);
    int64_t left = (int64_t)(SLd(st, 0x18) - (uint64_t)(uintptr_t)o2);
    if ((uint64_t)left <= 15 || (uint64_t)(s1 - s2) <= 8 || (uint64_t)(s4 - s5) <= 8) return 1;
    left -= 15;
    s1 -= 8; s4 -= 8;
    uint64_t b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0;   // bit offset into the current byte, then consumed bits
    bool more;
    do {
        b0 &= 7; b1 &= 7; b2 &= 7; b3 &= 7; b4 &= 7; b5 &= 7;
        left -= 15;
        uint64_t w0 = (HLd64(s0) | 0x8000000000000000ull) >> b0;
        uint64_t w1 = (__builtin_bswap64(HLd64(s1)) | 0x8000000000000000ull) >> b1;
        uint64_t w2 = (HLd64(s2) | 0x8000000000000000ull) >> b2;
        uint64_t w3 = (HLd64(s3) | 0x8000000000000000ull) >> b3;
        uint64_t w4 = (__builtin_bswap64(HLd64(s4)) | 0x8000000000000000ull) >> b4;
        uint64_t w5 = (HLd64(s5) | 0x8000000000000000ull) >> b5;
        // Each guest store writes 4 bytes: sym(s0), sym(s1), sym(s2), len(s2); the next store (3 bytes on) covers the last
        // one, so only the fifth leaves len(s2) behind, at out + 15. Byte stores give the same memory and keep the six
        // lookup chains in registers on x86.
#define HF_STEP(k)                                                                                                     \
        {                                                                                                              \
            const uint32_t e0 = lut[w0 & 0x7ff], e1 = lut[w1 & 0x7ff], e2 = lut[w2 & 0x7ff];                          \
            const uint32_t e3 = lut[w3 & 0x7ff], e4 = lut[w4 & 0x7ff], e5 = lut[w5 & 0x7ff];                          \
            o1[3 * k] = (uint8_t)(e0 >> 8); o1[3 * k + 1] = (uint8_t)(e1 >> 8); o1[3 * k + 2] = (uint8_t)(e2 >> 8);    \
            o2[3 * k] = (uint8_t)(e3 >> 8); o2[3 * k + 1] = (uint8_t)(e4 >> 8); o2[3 * k + 2] = (uint8_t)(e5 >> 8);    \
            if (k < 4) {                                                                                               \
                w0 >>= (e0 & 63); w1 >>= (e1 & 63); w2 >>= (e2 & 63); w3 >>= (e3 & 63); w4 >>= (e4 & 63); w5 >>= (e5 & 63); \
            } else {                                                                                                   \
                o1[15] = (uint8_t)e2; o2[15] = (uint8_t)e5;                                                            \
                b0 = __builtin_clzll(w0) + (e0 & 0xff); b1 = __builtin_clzll(w1) + (e1 & 0xff);                        \
                b2 = __builtin_clzll(w2) + (e2 & 0xff); b3 = __builtin_clzll(w3) + (e3 & 0xff);                        \
                b4 = __builtin_clzll(w4) + (e4 & 0xff); b5 = __builtin_clzll(w5) + (e5 & 0xff);                        \
            }                                                                                                          \
        }
        HF_STEP(0) HF_STEP(1) HF_STEP(2) HF_STEP(3) HF_STEP(4)
#undef HF_STEP
        s0 += b0 >> 3; s2 += b2 >> 3; s1 -= b1 >> 3;
        s3 += b3 >> 3; s5 += b5 >> 3; s4 -= b4 >> 3;
        o1 += 15; o2 += 15;
        // the guest's chain: gt(left) && s0 <= s2 && s2 <= s1 && s3 <= s5 && s5 <= s4 (s1, s4 still 8 below the pointer)
        more = left > 0 && s0 <= s2 && s2 <= s1 && s3 <= s5 && s5 <= s4;
    } while (more);
    SSt(st, 0x00, (uint64_t)(uintptr_t)o1);
    SSt(st, 0x08, (uint64_t)(uintptr_t)o2);
    s1 += 8; s4 += 8;
    b0 &= 7; b1 &= 7; b2 &= 7; b3 &= 7; b4 &= 7; b5 &= 7;
    if (s0 > s2 || s2 > s1 || s3 > s5 || s5 > s4) return 0;
    if (s1 == s2 && b1 + b2 != 0) return 0;
    if (s4 == s5 && b4 + b5 != 0) return 0;
    auto fwd = [&](const uint8_t *&p, uint64_t b, int slot) {
        if (b) { uint32_t v = *p++ >> b; SSt32(st, 0x60 + 4 * slot, v); SSt32(st, 0x78 + 4 * slot, (uint32_t)(8 - b)); }
    };
    auto bwd = [&](const uint8_t *&p, uint64_t b, int slot) {
        if (b) { uint32_t v = *--p >> b; SSt32(st, 0x60 + 4 * slot, v); SSt32(st, 0x78 + 4 * slot, (uint32_t)(8 - b)); }
    };
    fwd(s0, b0, 0); SSt(st, 0x30, (uint64_t)(uintptr_t)s0);
    bwd(s1, b1, 1); SSt(st, 0x38, (uint64_t)(uintptr_t)s1);
    fwd(s2, b2, 2); SSt(st, 0x40, (uint64_t)(uintptr_t)s2);
    fwd(s3, b3, 3); SSt(st, 0x48, (uint64_t)(uintptr_t)s3);
    bwd(s4, b4, 4); SSt(st, 0x50, (uint64_t)(uintptr_t)s4);
    fwd(s5, b5, 5); SSt(st, 0x58, (uint64_t)(uintptr_t)s5);
    return 1;
}

// ---- main+0x16680 (delta = false) and main+0x16ed0 (delta = true).
// lz = the LZ table block: [0x10] offsets (int32) [0x18] count, [0x20] lengths (int32) [0x28] count, [0x30] commands
// [0x38] count, [0x40] literals [0x48] count, [0x50] [0x58] a second offset stream that must be empty.
// dst..dst_end is this chunk's output, x2 the chunk start the guest measures from, base the lowest byte a match may read.
struct LzArgs {
    uint64_t lz, dst, x2, dst_end, base;
};
struct LzPlan {   // filled by LzHeader
    const int32_t *offs, *offsEnd, *lens, *lensEnd;
    const uint8_t *cmd, *cmdEnd, *lit, *litEnd;
    uint64_t lenEnd;   // address of the length stream end (the guest pads 64 bytes there)
    int64_t n;         // chunk size
    int64_t baseRel;   // base - dst (<= 0): the lowest position a match may read
};

inline uint64_t ULd(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }

// The guest's checks that do not depend on the streams' contents, plus what this code does not cover.
// 1 = go on, 0 = the guest returns 0 before writing anything (chunk under 16 bytes), -1 = run the guest.
inline int LzHeader(const LzArgs &a, LzPlan &p) {
    if ((int64_t)(a.dst_end - a.x2) < 16) return 0;
    const uint64_t offs = ULd(a.lz + 0x10), noffs = ULd(a.lz + 0x18), lens = ULd(a.lz + 0x20), nlens = ULd(a.lz + 0x28);
    const uint64_t cmd = ULd(a.lz + 0x30), ncmd = ULd(a.lz + 0x38), lit = ULd(a.lz + 0x40), nlit = ULd(a.lz + 0x48);
    if (ULd(a.lz + 0x58) != 0) return -1;
    if (noffs > (1u << 20) || nlens > (1u << 20) || ncmd > (1u << 20) || nlit > (1u << 20)) return -1;
    if (a.dst_end < a.dst || a.dst_end - a.dst > (1u << 24) || a.base > a.dst) return -1;
    const uint64_t lenEnd = lens + 4 * nlens, offsEnd = offs + 4 * noffs;
    if (offsEnd > lenEnd) return -1;   // the guest bounds the offset pointer by the length stream end
    // the guest pads the length stream before reading it: a stream overlapping its padding is not covered
    const uint64_t padLo = lenEnd, padHi = lenEnd + 64;
    auto overlaps = [&](uint64_t lo, uint64_t hi) { return lo < padHi && padLo < hi; };
    if (overlaps(offs, offsEnd) || overlaps(cmd, cmd + ncmd) || overlaps(lit, lit + nlit) || overlaps(a.base, a.dst_end)) return -1;
    // the guest reads its streams in words ahead of the output: streams inside the output window are not covered
    auto inOut = [&](uint64_t lo, uint64_t hi) { return lo < a.dst_end && a.base < hi; };
    if (inOut(offs, offsEnd) || inOut(lens, lenEnd + 64) || inOut(cmd, cmd + ncmd) || inOut(lit, lit + nlit)) return -1;
    p.offs = (const int32_t *)(uintptr_t)offs; p.offsEnd = (const int32_t *)(uintptr_t)offsEnd;
    p.lens = (const int32_t *)(uintptr_t)lens; p.lensEnd = (const int32_t *)(uintptr_t)lenEnd;
    p.cmd = (const uint8_t *)(uintptr_t)cmd; p.cmdEnd = p.cmd + ncmd;
    p.lit = (const uint8_t *)(uintptr_t)lit; p.litEnd = p.lit + nlit;
    p.lenEnd = lenEnd;
    p.n = (int64_t)(a.dst_end - a.dst);
    p.baseRel = (int64_t)a.base - (int64_t)a.dst;
    return 1;
}

// Walks the commands without writing: false if LzRun would stop early. reach = the lowest position read (<= 0), for
// the check mode's shadow buffer.
inline bool LzReach(const LzPlan &p, bool delta, int64_t &reach) {
    const int32_t *po = p.offs, *pl = p.lens;
    int64_t pos = 0, litLeft = p.litEnd - p.lit, last = -8;
    int32_t rec[4] = {-8, -8, -8, 0};
    reach = 0;
    for (const uint8_t *c = p.cmd; c < p.cmdEnd; c++) {
        const uint32_t f = *c;
        int64_t ll = f & 3;
        if (ll == 3) { if (pl >= p.lensEnd) return false; ll = *pl++; if (ll < 0) return false; }
        if (ll > p.n - 16 - pos || ll > litLeft) return false;
        if (delta && ll && pos + last < reach) reach = pos + last;
        pos += ll; litLeft -= ll;
        const uint32_t idx = f >> 6;
        if (idx == 3) { if (po >= p.offsEnd) return false; rec[3] = *po++; }
        const int32_t off = rec[idx];
        for (uint32_t i = idx; i > 0; i--) rec[i] = rec[i - 1];
        rec[0] = off;
        last = off;
        if (off > -8 || (int64_t)off < p.baseRel - pos) return false;
        if (pos + off < reach) reach = pos + off;
        int64_t ml = (f >> 2) & 15;
        if (ml == 15) { if (pl >= p.lensEnd) return false; ml = (int64_t)*pl++ + 14; if (ml < 0 || ml > p.n - 8 - pos) return false; }
        else ml += 2;
        pos += ml;
    }
    if (po != p.offsEnd || pl != p.lensEnd || pos > p.n || litLeft != p.n - pos) return false;
    if (delta && litLeft && pos + last < reach) reach = pos + last;
    return true;
}

inline uint64_t AddBytes8(uint64_t a, uint64_t b) {   // eight independent byte additions
    const uint64_t h = 0x8080808080808080ull;
    return ((a & ~h) + (b & ~h)) ^ ((a ^ b) & h);
}

// Decodes the chunk in place: out[0] is dst, out[-k] the earlier output (k up to -baseRel; in check mode a copy of it).
// Checks every command as the guest does (and offset <= -8, see the top); false = stopped (run the guest).
// Bytes past the current end are written ahead (word copies) only while they stay inside the chunk; they are always
// rewritten later, as in the guest.
template <bool Delta>
inline bool LzRun(const LzPlan &p, uint8_t *out) {
    const int32_t *po = p.offs, *pl = p.lens;
    const uint8_t *lit = p.lit, *litEnd = p.litEnd;
    const int64_t n = p.n;
    uint8_t *d = out, *const end = out + n;
    alignas(16) uint8_t tail[96];   // the last literals with zeros after them, so word reads never pass the stream end
    bool inTail = false;
    int32_t recBuf[8] = {0, 0, 0, -8, -8, -8, 0, 0}, *const rec = recBuf + 3;
    int64_t last = -8;
    uintptr_t litFast = (uintptr_t)litEnd - 48;   // below this an 8-byte literal read stays inside the stream
    if (p.litEnd - p.lit < 48) litFast = 0;
    auto litCopy = [&](int64_t ll) -> bool {
        if (ll > litEnd - lit) return false;
        if (!inTail && litEnd - lit < 48) {
            const size_t r = (size_t)(litEnd - lit);
            memset(tail, 0, sizeof tail);
            memcpy(tail, lit, r);
            lit = tail; litEnd = tail + r; inTail = true;
            litFast = (uintptr_t)tail + 48;
        }
        if (Delta) {
            int64_t i = 0;
            if (end - d >= ll + 8 && litEnd - lit >= ll + 8 - (inTail ? 40 : 0)) {
                for (; i < ll; i += 8) {
                    uint64_t x, y;
                    memcpy(&x, lit + i, 8); memcpy(&y, d + i + last, 8);
                    x = AddBytes8(x, y);
                    memcpy(d + i, &x, 8);
                }
            } else {
                for (; i < ll; i++) d[i] = (uint8_t)(lit[i] + d[i + last]);
            }
        } else {
            if (end - d >= ll + 16 && litEnd - lit >= ll + 16 - (inTail ? 40 : 0)) {
                int64_t i = 0;
                do { memcpy(d + i, lit + i, 16); i += 16; } while (i < ll);
            } else {
                memcpy(d, lit, (size_t)ll);
            }
        }
        d += ll; lit += ll;
        return true;
    };
    for (const uint8_t *c = p.cmd; c < p.cmdEnd; c++) {
        const uint32_t f = *c;
        int64_t ll = f & 3;
        if (ll == 3) { if (pl >= p.lensEnd) return false; ll = *pl++; if (ll < 0) return false; }
        if (ll > (end - 16) - d) return false;
        if (ll <= 8 && (uintptr_t)lit <= litFast && ll <= litEnd - lit) {   // one word, as the guest does
            uint64_t x;
            memcpy(&x, lit, 8);
            if (Delta) { uint64_t y; memcpy(&y, d + last, 8); x = AddBytes8(x, y); }
            memcpy(d, &x, 8);
            d += ll; lit += ll;
        } else if (!litCopy(ll)) return false;
        // recent offsets without branches, as the guest does: rec[3] always takes the next offset (only idx 3 keeps
        // it), the three entries below idx move up one, rec[0] = the chosen one; rec[-3..-1] are scratch
        const uint32_t idx = f >> 6;
        rec[3] = *po;   // readable: the offsets end at or before the length stream end, which has 64 padded bytes
        const int32_t off = rec[idx];
        { int32_t t[3]; memcpy(t, rec + idx - 3, 12); memcpy(rec + idx - 2, t, 12); }
        rec[0] = off;
        po += idx == 3;
        if (po > p.offsEnd) return false;
        last = off;
        if (off > -8 || (int64_t)off < p.baseRel - (d - out)) return false;
        const uint8_t *s = d + off;
        int64_t ml = (f >> 2) & 15;
        if (ml != 15) {   // 2..16 bytes; at least 16 bytes of room are left (the literal check above)
            uint64_t x;
            memcpy(&x, s, 8); memcpy(d, &x, 8);
            memcpy(&x, s + 8, 8); memcpy(d + 8, &x, 8);
            d += ml + 2;
            continue;
        }
        if (pl >= p.lensEnd) return false;
        ml = (int64_t)*pl++ + 14;
        if (ml < 0 || ml > (end - 8) - d) return false;
        uint8_t *const me = d + ml;
        if (off <= -16 && end - d >= ml + 16) {
            do { memcpy(d, s, 16); d += 16; s += 16; } while (d < me);
        } else if (end - d >= ml + 8) {
            do { uint64_t x; memcpy(&x, s, 8); memcpy(d, &x, 8); d += 8; s += 8; } while (d < me);
        } else {
            while (d < me) *d++ = *s++;
        }
        d = me;
    }
    if (po != p.offsEnd || pl != p.lensEnd || d > end || litEnd - lit != end - d) return false;
    const int64_t r = end - d;
    if (Delta) { for (int64_t i = 0; i < r; i++) d[i] = (uint8_t)(lit[i] + d[i + last]); }
    else memcpy(d, lit, (size_t)r);
    return true;
}

}   // namespace hotfn
