// SPDX-License-Identifier: MIT
// zlib-compatible CRC-32 (polynomial 0xEDB88320, `crc32(seed, buf, len)`: the seed and the result are the usual finished values, so
// calls chain) with a PCLMULQDQ fold for long buffers, project code (ours; the fold constants are the published ones from Gopal et al.,
// "Fast CRC Computation for Generic Polynomials Using PCLMULQDQ", Intel 2009, for the reflected 0x04C11DB7 polynomial).
// Falls back to a table loop without PCLMULQDQ / SSE4.1 and for the tail. Header only so the host mod and the unit test share it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <immintrin.h>
#include <intrin.h>

namespace crcclmul {

inline const uint32_t *Table() {
    static const struct T {
        uint32_t t[256];
        T() {
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
                t[i] = c;
            }
        }
    } tab;
    return tab.t;
}

// Table CRC on the raw register (no pre/post inversion).
inline uint32_t RawTable(uint32_t reg, const uint8_t *p, size_t n) {
    const uint32_t *t = Table();
    while (n--) reg = (reg >> 8) ^ t[(reg ^ *p++) & 0xff];
    return reg;
}

inline bool HasClmul() {
    static const bool ok = [] {
        int r[4];
        __cpuid(r, 1);
        return (r[2] & (1 << 1)) && (r[2] & (1 << 19));   // PCLMULQDQ and SSE4.1
    }();
    return ok;
}

// Fold buf (len >= 64 and a multiple of 16) into the raw register.
__attribute__((target("pclmul,sse4.1"))) inline uint32_t RawClmul(uint32_t crc, const uint8_t *buf, size_t len) {
    alignas(16) static const uint64_t k1k2[] = {0x0154442bd4ull, 0x01c6e41596ull};
    alignas(16) static const uint64_t k3k4[] = {0x01751997d0ull, 0x00ccaa009eull};
    alignas(16) static const uint64_t k5k0[] = {0x0163cd6124ull, 0x0000000000ull};
    alignas(16) static const uint64_t poly[] = {0x01db710641ull, 0x01f7011641ull};
    __m128i x0, x1, x2, x3, x4, x5, x6, x7, x8, y5, y6, y7, y8;
    x1 = _mm_loadu_si128((const __m128i *)(buf + 0x00));
    x2 = _mm_loadu_si128((const __m128i *)(buf + 0x10));
    x3 = _mm_loadu_si128((const __m128i *)(buf + 0x20));
    x4 = _mm_loadu_si128((const __m128i *)(buf + 0x30));
    x1 = _mm_xor_si128(x1, _mm_cvtsi32_si128((int)crc));
    x0 = _mm_load_si128((const __m128i *)k1k2);
    buf += 64;
    len -= 64;
    while (len >= 64) {
        x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
        x6 = _mm_clmulepi64_si128(x2, x0, 0x00);
        x7 = _mm_clmulepi64_si128(x3, x0, 0x00);
        x8 = _mm_clmulepi64_si128(x4, x0, 0x00);
        x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
        x2 = _mm_clmulepi64_si128(x2, x0, 0x11);
        x3 = _mm_clmulepi64_si128(x3, x0, 0x11);
        x4 = _mm_clmulepi64_si128(x4, x0, 0x11);
        y5 = _mm_loadu_si128((const __m128i *)(buf + 0x00));
        y6 = _mm_loadu_si128((const __m128i *)(buf + 0x10));
        y7 = _mm_loadu_si128((const __m128i *)(buf + 0x20));
        y8 = _mm_loadu_si128((const __m128i *)(buf + 0x30));
        x1 = _mm_xor_si128(x1, x5);
        x2 = _mm_xor_si128(x2, x6);
        x3 = _mm_xor_si128(x3, x7);
        x4 = _mm_xor_si128(x4, x8);
        x1 = _mm_xor_si128(x1, y5);
        x2 = _mm_xor_si128(x2, y6);
        x3 = _mm_xor_si128(x3, y7);
        x4 = _mm_xor_si128(x4, y8);
        buf += 64;
        len -= 64;
    }
    x0 = _mm_load_si128((const __m128i *)k3k4);
    x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
    x1 = _mm_xor_si128(x1, x2);
    x1 = _mm_xor_si128(x1, x5);
    x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
    x1 = _mm_xor_si128(x1, x3);
    x1 = _mm_xor_si128(x1, x5);
    x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
    x1 = _mm_xor_si128(x1, x4);
    x1 = _mm_xor_si128(x1, x5);
    while (len >= 16) {
        x2 = _mm_loadu_si128((const __m128i *)buf);
        x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
        x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
        x1 = _mm_xor_si128(x1, x2);
        x1 = _mm_xor_si128(x1, x5);
        buf += 16;
        len -= 16;
    }
    x2 = _mm_clmulepi64_si128(x1, x0, 0x10);
    x3 = _mm_setr_epi32(~0, 0, ~0, 0);
    x1 = _mm_srli_si128(x1, 8);
    x1 = _mm_xor_si128(x1, x2);
    x0 = _mm_loadl_epi64((const __m128i *)k5k0);
    x2 = _mm_srli_si128(x1, 4);
    x1 = _mm_and_si128(x1, x3);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_xor_si128(x1, x2);
    x0 = _mm_load_si128((const __m128i *)poly);
    x2 = _mm_and_si128(x1, x3);
    x2 = _mm_clmulepi64_si128(x2, x0, 0x10);
    x2 = _mm_and_si128(x2, x3);
    x2 = _mm_clmulepi64_si128(x2, x0, 0x00);
    x1 = _mm_xor_si128(x1, x2);
    return (uint32_t)_mm_extract_epi32(x1, 1);
}

// zlib crc32(seed, buf, len)
inline uint32_t Crc32(uint32_t seed, const uint8_t *p, size_t n) {
    uint32_t reg = ~seed;
    if (n >= 64 && HasClmul()) {
        const size_t bulk = n & ~(size_t)15;
        reg = RawClmul(reg, p, bulk);
        p += bulk;
        n -= bulk;
    }
    reg = RawTable(reg, p, n);
    return ~reg;
}

}  // namespace crcclmul
