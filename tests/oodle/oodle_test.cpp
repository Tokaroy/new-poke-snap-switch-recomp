// Kraken decode test for the Oodle hot routines in host/mods/hotfn_oodle.h (project code, ours).
//
// Decodes every stream of a local corpus (tests/oodle/make_corpus.py: real Oodle streams from the romfs DRP archives)
// twice with the generated guest decoder main+0x7240 (tests/oodle/emit_oodle.py): ref_* without hooks, hk_* with the
// SNAP_GEN_HOTFN hooks into host/mods/hotfn.cpp, in the SNAP_HOT_OODLE mode of the environment (1, check, 0). Checks the
// reference against the DRP chunk's CRC32, the hooked output and return value against the reference byte for byte, the
// guard bytes after the output, and in check mode the per-call native/guest comparison of hotfn.cpp (rt_hotfn_mismatches).
// Times both decoders (thread cycles, MB of output per second at 3 GHz-equivalent cycles reported as cycles/byte).
//
// Usage: oodle_test.exe <main.elf> <relocations.tsv> <corpus.bin> [repeat]
#include <windows.h>
#include <intrin.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include "cpu.h"
void ref_7100007240(Cpu *c);
void hk_7100007240(Cpu *c);
long rt_hotfn_mismatches();
long rt_hotfn_oodle_guest_calls();
volatile int g_fnprof;
void fnprof_hit(uint64_t a, uint64_t lr, uint64_t x0) { (void)a; (void)lr; (void)x0; }
void rt_inline_hook(Cpu *c, uint64_t ra, void (*fn)(Cpu *c)) { (void)ra; fn(c); }
void rt_dispatch(Cpu *c) {
    fprintf(stderr, "unexpected indirect call to 0x%llx\n", (unsigned long long)c->pc);
    exit(3);
}
void rt_udf(Cpu *c, uint64_t pc) { (void)c; fprintf(stderr, "udf at 0x%llx\n", (unsigned long long)pc); exit(3); }
// the libc imports the decoder calls through its PLT
void f_7100a365c0(Cpu *c) { memset((void *)(uintptr_t)c->x[0], (int)c->x[1], (size_t)c->x[2]); }
void f_7100a36600(Cpu *c) { memmove((void *)(uintptr_t)c->x[0], (const void *)(uintptr_t)c->x[1], (size_t)c->x[2]); }
void f_7100a36610(Cpu *c) { memcpy((void *)(uintptr_t)c->x[0], (const void *)(uintptr_t)c->x[1], (size_t)c->x[2]); }
}

namespace {

constexpr uint64_t kBase = 0x7100000000ull;

std::vector<uint8_t> ReadFile(const char *path) {
    std::vector<uint8_t> d;
    FILE *f = fopen(path, "rb");
    if (!f) return d;
    fseek(f, 0, SEEK_END); d.resize((size_t)_ftelli64(f)); fseek(f, 0, SEEK_SET);
    if (!d.empty() && fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    fclose(f);
    return d;
}

// maps the PT_LOAD segments at the guest base and applies the RELATIVE relocations (GOT slots of the Oodle plugin table)
bool MapMain(const char *elfPath, const char *relPath) {
    std::vector<uint8_t> elf = ReadFile(elfPath);
    if (elf.size() < 64) { fprintf(stderr, "cannot read %s\n", elfPath); return false; }
    uint64_t phoff; uint16_t phnum;
    memcpy(&phoff, &elf[0x20], 8); memcpy(&phnum, &elf[0x38], 2);
    uint64_t hi = 0;
    for (int i = 0; i < phnum; i++) {
        uint32_t type; uint64_t va, msz;
        memcpy(&type, &elf[phoff + 56 * i], 4); memcpy(&va, &elf[phoff + 56 * i + 16], 8); memcpy(&msz, &elf[phoff + 56 * i + 40], 8);
        if (type == 1 && va + msz > hi) hi = va + msz;
    }
    hi = (hi + 0xffff) & ~0xffffull;
    uint8_t *img = (uint8_t *)VirtualAlloc((void *)kBase, hi, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (img != (uint8_t *)kBase) { fprintf(stderr, "cannot map the main image\n"); return false; }
    for (int i = 0; i < phnum; i++) {
        uint32_t type; uint64_t off, va, fsz;
        memcpy(&type, &elf[phoff + 56 * i], 4); memcpy(&off, &elf[phoff + 56 * i + 8], 8);
        memcpy(&va, &elf[phoff + 56 * i + 16], 8); memcpy(&fsz, &elf[phoff + 56 * i + 32], 8);
        if (type == 1) memcpy(img + va, &elf[off], fsz);
    }
    FILE *f = fopen(relPath, "r");
    if (!f) { fprintf(stderr, "cannot read %s\n", relPath); return false; }
    char line[512];
    long n = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long off, add;
        if (sscanf(line, "0x%llx\tRELATIVE\t\t0x%llx", &off, &add) == 2 && off + 8 <= hi) {
            const uint64_t v = kBase + add;
            memcpy(img + off, &v, 8);
            n++;
        }
    }
    fclose(f);
    return n > 0;
}

uint32_t Crc32(const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; t[i] = c; }
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

uint64_t ThreadCycles() { ULONG64 v = 0; QueryThreadCycleTime(GetCurrentThread(), &v); return v; }

struct Decoder {
    Cpu *c;
    std::vector<uint8_t> stack, mem;
    Decoder() : stack(1 << 20), mem(16 << 20) {
        c = (Cpu *)_aligned_malloc(sizeof(Cpu), 64);
        memset(c, 0, sizeof(Cpu));
    }
    // OodleLZ_Decompress(src, srcLen, dst, dstLen, fuzzSafe 1, checkCRC 0, verbosity 0, dictBase 0, dictSize 0, callback 0,
    // callback data 0, decoder memory, its size, threadPhase 3): the game's call except for the decoder memory
    uint64_t Run(void (*fn)(Cpu *), const uint8_t *src, size_t srcLen, uint8_t *dst, size_t dstLen) {
        uint64_t sp = ((uint64_t)(uintptr_t)stack.data() + stack.size() - 0x100) & ~0xfull;
        uint64_t args[6] = {0, 0, 0, (uint64_t)(uintptr_t)mem.data(), (uint64_t)mem.size(), 3};
        memcpy((void *)(uintptr_t)sp, args, sizeof args);
        memset(c->x, 0, sizeof c->x);
        c->x[0] = (uint64_t)(uintptr_t)src; c->x[1] = srcLen; c->x[2] = (uint64_t)(uintptr_t)dst; c->x[3] = dstLen;
        c->x[4] = 1;
        c->x[30] = 0;
        c->sp = sp;
        fn(c);
        return c->x[0];
    }
};

}   // namespace

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: oodle_test <main.elf> <relocations.tsv> <corpus.bin> [repeat]\n"); return 2; }
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    if (!MapMain(argv[1], argv[2])) return 2;
    const int repeat = argc > 4 ? atoi(argv[4]) : 1;
    std::vector<uint8_t> corpus = ReadFile(argv[3]);
    if (corpus.empty()) { fprintf(stderr, "cannot read %s\n", argv[3]); return 2; }
    const char *mode = getenv("SNAP_HOT_OODLE");
    Decoder d;
    long streams = 0, crcBad = 0, diff = 0, guard = 0;
    double mb = 0;
    uint64_t cycRef = 0, cycHk = 0;
    constexpr size_t kGuard = 256;
    for (size_t pos = 0; pos + 12 <= corpus.size();) {
        uint32_t clen, dlen, crc;
        memcpy(&clen, &corpus[pos], 4); memcpy(&dlen, &corpus[pos + 4], 4); memcpy(&crc, &corpus[pos + 8], 4);
        pos += 12;
        if (pos + clen > corpus.size()) break;
        std::vector<uint8_t> src(corpus.begin() + pos, corpus.begin() + pos + clen);
        src.resize(clen + 64, 0);
        pos += clen;
        std::vector<uint8_t> a(dlen + kGuard, 0xa5), b(dlen + kGuard, 0x5a);
        uint64_t ra = 0, rb = 0;
        for (int r = 0; r < repeat; r++) {
            uint64_t t0 = ThreadCycles();
            ra = d.Run(ref_7100007240, src.data(), clen, a.data(), dlen);
            uint64_t t1 = ThreadCycles();
            rb = d.Run(hk_7100007240, src.data(), clen, b.data(), dlen);
            uint64_t t2 = ThreadCycles();
            cycRef += t1 - t0; cycHk += t2 - t1;
        }
        streams++;
        mb += repeat * dlen / 1048576.0;
        const bool crcOk = ra == dlen && Crc32(a.data(), dlen) == crc;
        if (!crcOk) { crcBad++; fprintf(stderr, "stream %ld: reference decode returned %llu of %u, crc %s\n", streams, (unsigned long long)ra, dlen, crcOk ? "ok" : "bad"); }
        if (ra != rb || memcmp(a.data(), b.data(), dlen) != 0) {
            diff++;
            size_t i = 0;
            while (i < dlen && a[i] == b[i]) i++;
            if (diff <= 5) fprintf(stderr, "stream %ld: hooked decode differs (ret %llu vs %llu, first byte %zu of %u)\n", streams,
                                   (unsigned long long)ra, (unsigned long long)rb, i, dlen);
        }
        for (size_t i = 0; i < kGuard; i++) {
            if (a[dlen + i] != 0xa5 || b[dlen + i] != 0x5a) { guard++; break; }
        }
    }
    const long mism = rt_hotfn_mismatches(), toGuest = rt_hotfn_oodle_guest_calls();
    printf("SNAP_HOT_OODLE=%s: %ld streams, %.1f MB decoded x2; reference crc failures %ld, hooked output differences %ld, "
           "guard overwrites %ld, check mismatches %ld, calls left to the guest %ld\n",
           mode ? mode : "(unset)", streams, mb, crcBad, diff, guard, mism, toGuest);
    printf("  decode: guest %.2f cycles/byte, hooked %.2f cycles/byte, %.2fx\n", cycRef / (mb * 1048576.0), cycHk / (mb * 1048576.0),
           (double)cycRef / (double)(cycHk ? cycHk : 1));
    return (crcBad || diff || guard || mism) ? 1 : 0;
}
