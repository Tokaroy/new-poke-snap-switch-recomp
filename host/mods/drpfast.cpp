// DRP keystream decrypt on the host (perf pass 11, 2026-11-14; project code, ours).
//
// The DRP chunk reader 0x5ec890 decrypts a chunk one 32-bit word at a time: for each word it calls the guest random number
// function 0x5ec660 (xorshift-like, state = 4 words at x21) with the bound 0xffffffff, which draws two raw steps per word, and
// XORs the result into the word. That call per word was about 2 s of loader CPU per course start (ResourceDecrypt thread).
// The inline hook rt_drp_decrypt runs at 0x5ec9f0 (`cmp x28, x23`, x28 = first word, x23 = end) and does the whole loop on the
// host, then sets x28 = x23 so the guest loop is skipped. It leaves the state words and the data exactly as the guest loop
// would; the caller does not read the scratch registers the loop leaves behind (the next instruction after the loop is a bl).
//
// SNAP_DRP_FASTDEC=0 keeps the guest loop. SNAP_DRP_FASTDEC=check runs the host loop on a copy, lets the guest loop run, and
// compares the data and state at 0x5eca1c (rt_drp_decrypt_check); it prints the first mismatches and a count at each 256th chunk.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "loader.h"

namespace {

int Mode() {   // 0 off, 1 on, 2 check
    static const int m = [] {
        const char *e = getenv("SNAP_DRP_FASTDEC");
        if (!e || !*e) return 1;
        if (!strcmp(e, "check")) return 2;
        return e[0] == '0' ? 0 : 1;
    }();
    return m;
}

inline uint32_t Ld(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
inline void St(uint64_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }

// One raw step of 0x5ec660's generator: returns the new state word 0 (s3' = s2, s2' = s1, s1' = s0).
inline uint32_t Step(uint32_t s[4]) {
    const uint32_t a = s[3] ^ (s[3] << 11);
    const uint32_t t = s[0] ^ (uint32_t)((int32_t)s[0] >> 19) ^ a;
    const uint32_t n = t ^ (uint32_t)((int32_t)a >> 8);
    s[3] = s[2]; s[2] = s[1]; s[1] = s[0]; s[0] = n;
    return n;
}

// 0x5ec660 with bound 0xffffffff: low 31 bits from one step, bit 31 from bit 30 of the next (the bound-1 draw, divisor 2^30).
inline uint32_t Word(uint32_t s[4]) {
    const uint32_t lo = Step(s) & 0x7fffffffu;
    const uint32_t hi = (Step(s) & 0x7fffffffu) >> 30;
    return lo | hi << 31;
}

void Run(uint32_t s[4], uint8_t *p, uint8_t *end) {
    for (; p != end; p += 4) {
        uint32_t w; memcpy(&w, p, 4);
        w ^= Word(s);
        memcpy(p, &w, 4);
    }
}

thread_local std::vector<uint8_t> t_want;   // check mode: expected data and state for this thread's pending chunk
thread_local uint32_t t_wantState[4];
thread_local uint64_t t_begin = 0, t_stateAddr = 0;
std::atomic<long> g_chunks{0}, g_bad{0};

}   // namespace

extern "C" void rt_drp_decrypt(Cpu *c) {
    const int m = Mode();
    if (!m) return;
    const uint64_t b = c->x[28], e = c->x[23], st = c->x[21];
    if ((uint32_t)c->x[22] != 0xffffffffu || e <= b || ((e - b) & 3)) return;   // not the shape we know: leave it to the guest
    uint32_t s[4] = {Ld(st), Ld(st + 4), Ld(st + 8), Ld(st + 12)};
    if (m == 2) {
        t_want.assign((const uint8_t *)(uintptr_t)b, (const uint8_t *)(uintptr_t)e);
        Run(s, t_want.data(), t_want.data() + t_want.size());
        memcpy(t_wantState, s, sizeof s);
        t_begin = b; t_stateAddr = st;
        return;   // the guest loop runs; rt_drp_decrypt_check compares
    }
    Run(s, (uint8_t *)(uintptr_t)b, (uint8_t *)(uintptr_t)e);
    for (int i = 0; i < 4; i++) St(st + 4 * i, s[i]);
    c->x[28] = e;   // the `cmp x28, x23` that follows sees equal and skips the guest loop
}

extern "C" void rt_drp_decrypt_check(Cpu *c) {
    if (Mode() != 2 || !t_begin) return;
    const uint8_t *got = (const uint8_t *)(uintptr_t)t_begin;
    bool bad = memcmp(got, t_want.data(), t_want.size()) != 0;
    for (int i = 0; i < 4; i++) bad |= Ld(t_stateAddr + 4 * i) != t_wantState[i];
    const long n = ++g_chunks;
    if (bad && ++g_bad <= 5) fprintf(stderr, "[drpfast] MISMATCH chunk %ld at 0x%llx, %zu bytes\n", n, (unsigned long long)t_begin, t_want.size());
    if ((n & 255) == 0) fprintf(stderr, "[drpfast] check: %ld chunks, %ld mismatches\n", n, g_bad.load());
    t_begin = 0;
}
