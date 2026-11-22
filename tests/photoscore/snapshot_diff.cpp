// Offline differential test over function-entry snapshots (project code, ours; no game code in this file).
//
// Replays every call recorded by host/mods/pscap.cpp (SNAP_PSCAP) twice on the same memory: once through the lifted guest function
// and once through a second implementation of the same function (a readable rewrite), then compares what the caller can see:
// every byte the call wrote at or above its entry sp or outside the stack (heap, output structs), plus the return registers named
// on the command line. Built by tests/photoscore/run_snapshot_diff.py, which supplies both implementations:
//   extern "C" GuestFn lifted_fn(uint64_t main_off);     the lifted function (generated main, extracted into build/, never committed)
//   extern "C" GuestFn readable_fn(uint64_t main_off);   the rewrite (kept outside the repository when it is game-derived)
// Either may return null for an offset it does not cover; such calls are counted as skipped.
//
// Usage: snapshot_diff <main.elf> <capture.psc> [--ret <off>:<x0|w0|s0>[,...]] [--only <off>[,...]] [--csv <file>] [--verbose n]
//                      [--data-sym <imported data symbol>=<float>]...
// Exit code 0 only when every compared call matched and none faulted.
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "cpu.h"
typedef void (*GuestFnPtr)(Cpu *);
GuestFnPtr lifted_fn(uint64_t main_off);
GuestFnPtr readable_fn(uint64_t main_off);
}

namespace {

constexpr uint64_t kBase = 0x7100000000ull;
uint64_t g_imageEnd = 0x7100000000ull;   // end of the mapped main image (set by MapMain)
constexpr uint64_t kPage = 4096;

struct Call {
    uint64_t fn, lr, x[31], sp, v[8][2], fpcr, seq;
    std::vector<std::pair<uint64_t, uint64_t>> pages;   // addr, page id
};

std::unordered_map<uint64_t, std::vector<uint8_t>> g_page_data;   // id -> bytes
std::vector<Call> g_calls;

bool ReadCapture(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return false; }
    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "PSC1", 4)) { fprintf(stderr, "%s: not a PSC1 capture\n", path); return false; }
    for (;;) {
        int k = fgetc(f);
        if (k == EOF) break;
        if (k == 'P') {
            uint64_t addr, id;
            std::vector<uint8_t> b(kPage);
            if (fread(&addr, 8, 1, f) != 1 || fread(&id, 8, 1, f) != 1 || fread(b.data(), 1, kPage, f) != kPage) break;
            g_page_data[id] = std::move(b);
        } else if (k == 'C') {
            Call c{};
            uint32_t n = 0;
            if (fread(&c.fn, 8, 1, f) != 1 || fread(&c.lr, 8, 1, f) != 1 || fread(c.x, 8, 31, f) != 31 || fread(&c.sp, 8, 1, f) != 1 ||
                fread(c.v, 16, 8, f) != 8 || fread(&c.fpcr, 8, 1, f) != 1 || fread(&c.seq, 8, 1, f) != 1 || fread(&n, 4, 1, f) != 1) break;
            c.pages.resize(n);
            bool ok = true;
            for (auto &p : c.pages) ok = ok && fread(&p.first, 8, 1, f) == 1 && fread(&p.second, 8, 1, f) == 1;
            if (!ok) break;
            g_calls.push_back(std::move(c));
        } else {
            fprintf(stderr, "%s: bad record tag %d (truncated capture?)\n", path, k);
            break;
        }
    }
    fclose(f);
    return true;
}

// Relocations, as the loader applies them: RELATIVE = base + addend; a symbol defined in main = base + value + addend; a symbol
// imported from the SDK points at a zeroed 64-byte slot of its own, or at a value given with --data-sym <name>=<float>.
std::map<std::string, float> g_data_sym_values;
std::map<std::string, std::array<uint8_t, 64>> g_data_sym_slots;

void ApplyRelocations(const std::vector<uint8_t> &elf, uint8_t *img) {
    uint64_t phoff; uint16_t phnum;
    memcpy(&phoff, &elf[0x20], 8); memcpy(&phnum, &elf[0x38], 2);
    uint64_t dyn = 0;
    for (int i = 0; i < phnum; i++) {
        uint32_t type; memcpy(&type, &elf[phoff + 56 * i], 4);
        if (type == 2) memcpy(&dyn, &elf[phoff + 56 * i + 16], 8);
    }
    if (!dyn) return;
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, symtab = 0, strtab = 0;
    for (const uint64_t *d = (const uint64_t *)(img + dyn); d[0]; d += 2) {
        switch (d[0]) {
        case 7: rela = d[1]; break;
        case 8: relasz = d[1]; break;
        case 23: jmprel = d[1]; break;
        case 2: pltsz = d[1]; break;
        case 6: symtab = d[1]; break;
        case 5: strtab = d[1]; break;
        }
    }
    auto apply = [&](uint64_t at, uint64_t size) {
        for (uint64_t o = 0; o + 24 <= size; o += 24) {
            uint64_t off, info; int64_t add;
            memcpy(&off, img + at + o, 8); memcpy(&info, img + at + o + 8, 8); memcpy(&add, img + at + o + 16, 8);
            uint32_t type = uint32_t(info), sym = uint32_t(info >> 32);
            uint64_t value;
            if (type == 1027) {                                   // R_AARCH64_RELATIVE
                value = kBase + add;
            } else if (type == 257 || type == 1025 || type == 1026) {   // ABS64, GLOB_DAT, JUMP_SLOT
                const uint8_t *st = img + symtab + 24ull * sym;
                uint32_t name; uint16_t shndx; uint64_t sv;
                memcpy(&name, st, 4); memcpy(&shndx, st + 6, 2); memcpy(&sv, st + 8, 8);
                if (shndx != 0) {
                    value = kBase + sv + add;
                } else if (type == 1026) {
                    continue;                                     // imported functions go through the PLT stubs instead
                } else {
                    std::string n = (const char *)(img + strtab + name);
                    auto &slot = g_data_sym_slots[n];
                    auto it = g_data_sym_values.find(n);
                    if (it != g_data_sym_values.end()) memcpy(slot.data(), &it->second, 4);
                    value = (uint64_t)(uintptr_t)slot.data() + add;
                }
            } else {
                continue;
            }
            memcpy(img + off, &value, 8);
        }
    };
    apply(rela, relasz);
    apply(jmprel, pltsz);
}

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
    g_imageEnd = kBase + hi;
    uint8_t *img = (uint8_t *)VirtualAlloc((void *)kBase, hi, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (img != (uint8_t *)kBase) { fprintf(stderr, "cannot map the main image at 0x%llx\n", (unsigned long long)kBase); return false; }
    for (int i = 0; i < phnum; i++) {
        uint32_t type; uint64_t off, va, fsz;
        memcpy(&type, &elf[phoff + 56 * i], 4); memcpy(&off, &elf[phoff + 56 * i + 8], 8);
        memcpy(&va, &elf[phoff + 56 * i + 16], 8); memcpy(&fsz, &elf[phoff + 56 * i + 32], 8);
        if (type == 1) memcpy(img + va, &elf[off], fsz);
    }
    ApplyRelocations(elf, img);
    return true;
}

// Guest pages live at their own addresses (the game identity-maps guest memory). Each page is committed on first use.
std::set<uint64_t> g_committed;
bool EnsurePage(uint64_t p) {
    if (g_committed.count(p)) return true;
    MEMORY_BASIC_INFORMATION mi{};
    VirtualQuery((void *)p, &mi, sizeof mi);
    if (mi.State == MEM_COMMIT) {
        if (p >= kBase && p < g_imageEnd) { g_committed.insert(p); return true; }   // inside the mapped main image
        fprintf(stderr, "guest page %#llx collides with test-process memory\n", (unsigned long long)p);
        return false;
    }
    if (mi.State == MEM_FREE && !VirtualAlloc((void *)(p & ~0xffffull), 0x10000, MEM_RESERVE, PAGE_READWRITE)) {
        fprintf(stderr, "cannot reserve %#llx\n", (unsigned long long)p);
        return false;
    }
    if (!VirtualAlloc((void *)p, kPage, MEM_COMMIT, PAGE_READWRITE)) {
        fprintf(stderr, "cannot commit %#llx\n", (unsigned long long)p);
        return false;
    }
    g_committed.insert(p);
    return true;
}

// Scratch memory for guest allocations made during a call (runner-provided allocator stubs use it).
constexpr uint64_t kArena = 0x0000060000000000ull, kArenaSize = 64ull << 20;
uint64_t g_arena_next = kArena;
}  // namespace

extern "C" uint64_t snapshot_diff_alloc(uint64_t n) {
    uint64_t a = (g_arena_next + 15) & ~15ull;
    if (a + n > kArena + kArenaSize) return 0;
    g_arena_next = a + n;
    return a;
}

namespace {

void LoadPages(const Call &c) {
    for (auto &p : c.pages) memcpy((void *)p.first, g_page_data[p.second].data(), kPage);
    g_arena_next = kArena;
    memset((void *)kArena, 0, 1 << 20);
}

std::vector<uint8_t> SavePages(const Call &c) {
    std::vector<uint8_t> out(c.pages.size() * kPage);
    for (size_t i = 0; i < c.pages.size(); i++) memcpy(&out[i * kPage], (void *)c.pages[i].first, kPage);
    return out;
}

void SetCpu(Cpu *cpu, const Call &c) {
    memset(cpu, 0, sizeof *cpu);
    memcpy(cpu->x, c.x, sizeof c.x);
    cpu->x[30] = c.lr;
    cpu->sp = c.sp;
    for (int i = 0; i < 8; i++) memcpy(&cpu->v[i], c.v[i], 16);
    cpu->fpcr = c.fpcr;
}

uint64_t g_fault_addr;
LONG CALLBACK OnFault(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) g_fault_addr = e->ExceptionRecord->ExceptionInformation[1];
    return EXCEPTION_CONTINUE_SEARCH;
}

// Runs fn under SEH so a read of memory the capture missed is reported instead of crashing the test.
bool RunGuarded(GuestFnPtr fn, Cpu *cpu) {
    g_fault_addr = 0;
    __try { fn(cpu); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

struct RetSpec { char kind; };   // 'x' x0, 'w' w0, 's' s0

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: snapshot_diff <main.elf> <capture.psc> [--ret off:x0|w0|s0,...] [--only off,...] [--csv file]\n"); return 2; }
    std::map<uint64_t, char> rets;
    std::set<uint64_t> only;
    const char *csv_path = nullptr;
    int verbose = 3;
    for (int i = 3; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        const char *s = argv[i + 1];
        if (k == "--data-sym") {
            std::string v = s;
            size_t eq = v.find('=');
            g_data_sym_values[v.substr(0, eq)] = strtof(v.c_str() + eq + 1, nullptr);
        } else if (k == "--ret") {
            while (*s) {
                char *end;
                uint64_t off = strtoull(s, &end, 16);
                if (*end == ':') { rets[off] = end[1]; end += 3; }
                s = *end == ',' ? end + 1 : end;
                if (!*end) break;
            }
        } else if (k == "--only") {
            while (*s) { char *end; only.insert(strtoull(s, &end, 16)); s = *end == ',' ? end + 1 : end; if (!*end) break; }
        } else if (k == "--csv") csv_path = s;
        else if (k == "--verbose") verbose = atoi(s);
    }
    AddVectoredExceptionHandler(1, OnFault);
    if (!MapMain(argv[1]) || !ReadCapture(argv[2])) return 2;
    if (!VirtualAlloc((void *)kArena, kArenaSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) { fprintf(stderr, "cannot map the arena\n"); return 2; }
    for (auto &c : g_calls)
        for (auto &p : c.pages)
            if (!EnsurePage(p.first)) return 2;
    FILE *csv = csv_path ? fopen(csv_path, "w") : nullptr;
    if (csv) fprintf(csv, "seq,fn,lr,status,ret_lifted,ret_readable,out0,out1,out2,out3,out4,out5,out6,out7\n");
    Cpu *a = (Cpu *)_aligned_malloc(sizeof(Cpu), 64), *b = (Cpu *)_aligned_malloc(sizeof(Cpu), 64);
    std::map<uint64_t, std::array<long, 4>> per;   // fn -> matched, mismatched, faulted, skipped
    for (auto &c : g_calls) {
        if (!only.empty() && !only.count(c.fn)) continue;
        auto &tally = per[c.fn];
        GuestFnPtr lf = lifted_fn(c.fn), rf = readable_fn(c.fn);
        if (!lf || !rf) { tally[3]++; continue; }
        LoadPages(c);
        SetCpu(a, c);
        bool ok_a = RunGuarded(lf, a);
        uint64_t fault_a = g_fault_addr;
        std::vector<uint8_t> mem_a = SavePages(c);
        LoadPages(c);
        SetCpu(b, c);
        bool ok_b = RunGuarded(rf, b);
        uint64_t fault_b = g_fault_addr;
        std::vector<uint8_t> mem_b = SavePages(c);
        const char *status = "match";
        std::string why;
        if (!ok_a || !ok_b) {
            status = "fault";
            char buf[160];
            snprintf(buf, sizeof buf, "lifted %s (addr %#llx), readable %s (addr %#llx)", ok_a ? "ok" : "faulted", (unsigned long long)fault_a,
                     ok_b ? "ok" : "faulted", (unsigned long long)fault_b);
            why = buf;
        } else {
            for (size_t i = 0; i < c.pages.size() && why.empty(); i++) {
                uint64_t p = c.pages[i].first;
                for (uint64_t o = 0; o < kPage; o++) {
                    uint64_t addr = p + o;
                    bool scratch = addr < c.sp && addr >= c.sp - 0x100000;   // the callee's own frames
                    if (!scratch && mem_a[i * kPage + o] != mem_b[i * kPage + o]) {
                        char buf[160];
                        snprintf(buf, sizeof buf, "memory differs at %#llx (sp%+lld): lifted %02x readable %02x", (unsigned long long)addr,
                                 (long long)(addr - c.sp), mem_a[i * kPage + o], mem_b[i * kPage + o]);
                        why = buf;
                        break;
                    }
                }
            }
            auto r = rets.find(c.fn);
            if (why.empty() && r != rets.end()) {
                uint64_t va = r->second == 's' ? (uint32_t)a->v[0] : r->second == 'w' ? (uint32_t)a->x[0] : a->x[0];
                uint64_t vb = r->second == 's' ? (uint32_t)b->v[0] : r->second == 'w' ? (uint32_t)b->x[0] : b->x[0];
                if (va != vb) {
                    char buf[120];
                    snprintf(buf, sizeof buf, "return differs: lifted %#llx readable %#llx", (unsigned long long)va, (unsigned long long)vb);
                    why = buf;
                }
            }
            if (!why.empty()) status = "mismatch";
        }
        tally[status[0] == 'm' && status[1] == 'a' ? 0 : status[0] == 'f' ? 2 : 1]++;
        if (!why.empty() && verbose-- > 0)
            fprintf(stderr, "seq %llu fn main+%#llx from main+%#llx: %s\n", (unsigned long long)c.seq, (unsigned long long)c.fn,
                    (unsigned long long)(c.lr - kBase), why.c_str());
        if (csv) {
            auto r = rets.find(c.fn);
            char kind = r != rets.end() ? r->second : 'w';
            uint64_t ra = kind == 's' ? (uint32_t)a->v[0] : (uint32_t)a->x[0], rb = kind == 's' ? (uint32_t)b->v[0] : (uint32_t)b->x[0];
            fprintf(csv, "%llu,%llx,%llx,%s,%llx,%llx", (unsigned long long)c.seq, (unsigned long long)c.fn, (unsigned long long)(c.lr - kBase), status,
                    (unsigned long long)ra, (unsigned long long)rb);
            // x1 is the output block of the scoring functions; dump its first 8 words from the lifted run for the result join
            uint32_t w[8] = {};
            uint64_t out = c.x[1];
            for (size_t i = 0; i < c.pages.size(); i++)
                if (c.pages[i].first == (out & ~(kPage - 1)) && (out & (kPage - 1)) + 32 <= kPage) memcpy(w, &mem_a[i * kPage + (out & (kPage - 1))], 32);
            for (int k = 0; k < 8; k++) fprintf(csv, ",%08x", w[k]);
            fprintf(csv, "\n");
        }
    }
    if (csv) fclose(csv);
    long bad = 0;
    for (auto &[fn, t] : per) {
        printf("main+%#llx: %ld matched, %ld mismatched, %ld faulted, %ld skipped\n", (unsigned long long)fn, t[0], t[1], t[2], t[3]);
        bad += t[1] + t[2];
    }
    printf("%zu calls in capture, %s\n", g_calls.size(), bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
