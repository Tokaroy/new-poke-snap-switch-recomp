// DRP chunk CRC32 on the host (perf pass 17, 2026-11-15; project code, ours).
//
// The guest function 0x15fe90 is zlib's crc32(seed = x0, buf = x1, len = x2) built on the ARM crc32 instruction (our CRC32 intrinsic
// in runtime/simd.h, slice-by-8). The DRP chunk reader calls it over every chunk it loads; that was about 17% of the loader threads'
// CPU. The inline hook rt_crc32 runs at the function entry and, for len >= 64, computes the whole CRC with a PCLMULQDQ fold
// (runtime/crc32_clmul.h, bit-exact against the byte-wise reference, tests\hle\run_crc_test.ps1), then sets x0 = result,
// x1 += len, x2 = 0. The guest code that follows then sees fewer than 8 bytes left, none, and returns ~~x0 = x0. Shorter buffers
// and machines without PCLMULQDQ run the guest loop. Scratch registers x8-x13 are left as the guest would not have left them;
// they are caller-saved and the callers (the chunk reader) do not read them across the call.
//
// NOT HOOKED IN (no wall-clock gain): to re-add, put `BASE + 0x15fe90: 'rt_crc32', BASE + 0x15ff68: 'rt_crc32_check'` into MAIN_INLINE_HOOKS
// (recomp/snap_gen.py), regenerate main and rebuild build\eh\main (PGO data for f_710015fe90 stops matching), then set SNAP_CRC_CLMUL=1.
// Off by default (it saves about 1.2 s of loader CPU per course start but the load takes the same wall time).
// SNAP_CRC_CLMUL=1 turns it on, unset or 0 keeps the guest loop. SNAP_CRC_CLMUL=check computes the host CRC, lets the guest loop run, and compares the
// guest result at its return (rt_crc32_check at 0x15ff68); it prints the first mismatches and a count at each 4096th call.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "crc32_clmul.h"
#include "loader.h"

namespace {

int Mode() {   // 0 off, 1 on, 2 check
    static const int m = [] {
        const char *e = getenv("SNAP_CRC_CLMUL");
        if (!e || !*e) return 0;   // off by default: -1.2 s of loader CPU per course start, but no wall-clock gain (analysis\perf\results.md)
        if (!strcmp(e, "check")) return 2;
        return e[0] == '0' ? 0 : 1;
    }();
    return m;
}

thread_local uint32_t t_want = 0;
thread_local bool t_pending = false;
std::atomic<long> g_calls{0}, g_bad{0};

}   // namespace

extern "C" void rt_crc32(Cpu *c) {
    const int m = Mode();
    if (!m) return;
    const uint64_t len = c->x[2];
    if (len < 64 || !crcclmul::HasClmul()) return;
    const uint32_t r = crcclmul::Crc32((uint32_t)c->x[0], (const uint8_t *)(uintptr_t)c->x[1], (size_t)len);
    if (m == 2) {
        t_want = r;
        t_pending = true;
        return;   // the guest loop runs; rt_crc32_check compares
    }
    c->x[0] = r;
    c->x[1] += len;
    c->x[2] = 0;
}

extern "C" void rt_crc32_check(Cpu *c) {
    if (Mode() != 2 || !t_pending) return;
    t_pending = false;
    const long n = ++g_calls;
    const uint32_t got = (uint32_t)~(uint32_t)c->x[8];   // the guest's return value (x0 = ~x8 follows)
    if (got != t_want && ++g_bad <= 5) fprintf(stderr, "[crcfast] MISMATCH call %ld: guest %08x host %08x\n", n, got, t_want);
    if ((n & 4095) == 0) fprintf(stderr, "[crcfast] check: %ld long calls, %ld mismatches\n", n, g_bad.load());
}
