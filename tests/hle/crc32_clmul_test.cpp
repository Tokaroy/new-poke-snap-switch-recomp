// Bit-exact check of runtime/crc32_clmul.h against the guest's CRC32 (the ARM crc32 instruction, as runtime/simd.h implements it:
// reflected polynomial 0xEDB88320, byte by byte) and against zlib-style chaining. Run: tests\hle\run_crc_test.ps1
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "crc32_clmul.h"

// Reference = the guest semantics: acc = ~seed; per byte acc ^= b then 8 shifts; result ~acc (what f_710015fe90 does).
static uint32_t Ref(uint32_t seed, const uint8_t *p, size_t n) {
    uint32_t acc = ~seed;
    for (size_t i = 0; i < n; i++) {
        acc ^= p[i];
        for (int k = 0; k < 8; k++) acc = (acc >> 1) ^ (0xEDB88320u & (0u - (acc & 1)));
    }
    return ~acc;
}

int main() {
    std::mt19937_64 rng(12345);
    std::vector<uint8_t> big(1 << 22);
    for (auto &b : big) b = (uint8_t)rng();
    long checks = 0, bad = 0;
    auto check = [&](uint32_t seed, size_t off, size_t n) {
        const uint32_t want = Ref(seed, big.data() + off, n), got = crcclmul::Crc32(seed, big.data() + off, n);
        checks++;
        if (want != got && ++bad <= 10) std::printf("MISMATCH seed %08x off %zu len %zu: want %08x got %08x\n", seed, off, n, want, got);
    };
    std::printf("pclmul available: %d\n", (int)crcclmul::HasClmul());
    // every length 0..300 at every alignment 0..16, a few seeds
    for (size_t n = 0; n <= 300; n++)
        for (size_t off = 0; off <= 16; off++)
            for (uint32_t seed : {0u, 1u, 0xffffffffu, 0xdeadbeefu}) check(seed, off, n);
    // random lengths, offsets and seeds
    for (int i = 0; i < 3000; i++) check((uint32_t)rng(), rng() % 64, rng() % (1 << 16));
    for (int i = 0; i < 40; i++) check((uint32_t)rng(), rng() % 4096, rng() % (1 << 20));
    // chaining: crc(crc(a), b) == crc(a+b) at random split points
    for (int i = 0; i < 2000; i++) {
        const size_t off = rng() % 100, n = rng() % 20000, cut = n ? rng() % (n + 1) : 0;
        const uint32_t s = (uint32_t)rng();
        const uint32_t whole = crcclmul::Crc32(s, big.data() + off, n);
        const uint32_t part = crcclmul::Crc32(crcclmul::Crc32(s, big.data() + off, cut), big.data() + off + cut, n - cut);
        checks++;
        if (whole != part || whole != Ref(s, big.data() + off, n)) {
            bad++;
            std::printf("CHAIN MISMATCH off %zu n %zu cut %zu\n", off, n, cut);
        }
    }
    std::printf("%ld checks, %ld mismatches\n", checks, bad);
    // throughput (informational)
    auto t0 = std::chrono::steady_clock::now();
    uint32_t acc = 0;
    for (int i = 0; i < 100; i++) acc = crcclmul::Crc32(acc, big.data(), big.size());
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("clmul %.1f GB/s (acc %08x)\n", 100.0 * big.size() / s / 1e9, acc);
    return bad ? 1 : 0;
}
