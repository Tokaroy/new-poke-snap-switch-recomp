// Function-entry memory snapshots for offline differential tests (project code, ours; developer tool, off unless set).
//
// SNAP_PSCAP=<file>   turns it on and names the output file (binary, format below). Needs the function-entry hook, so also set
//                     SNAP_FNPROF_KEEP=1. Nothing here runs when SNAP_PSCAP is unset (one flag test per hooked entry).
// SNAP_PSCAP_FNS=<spec>[,<spec>...]  the lifted functions to snapshot on entry. <spec> = <main offset hex>[:<flags>], flags:
//                     t   only when another listed function without 't' was entered since this one's last snapshot (for a
//                         per-frame update function whose state is wanted right after the others ran)
//                     d<n> pointer depth from the registers and the stack (default 1), w<hex> bytes copied after each address
//                     found (default 0x400), m<n> snapshots of this function at most (default 3000),
//                     l<lo hex>-<hi hex> only when the return address is in [lo, hi) (main offsets)
// SNAP_PSCAP_ROOTS=<root>[;<root>...]  extra starting points, walked deeper than the default ones:
//                     <fn hex>@x<n>[><off hex>...][+<add hex>]:w<hex>[:d<n>][:map]
//                     x<n> = register; each '>off' loads the pointer at (address + off); '+add' adds at the end. 'map' treats the
//                     address as a libc++ unordered_map (bucket pointer, bucket count, first node) and copies the whole bucket array
//                     and every node (w bytes each) before the pointer walk.
// SNAP_PSCAP_STACK=<below>,<above>   guest stack bytes copied around sp (default 0x8000,0x2000).
//
// The walk: each copied window is scanned for aligned 8-byte values that are readable addresses; each one adds a window there,
// one level deeper, breadth first, up to the depth and a page budget. It knows nothing about the game's data. Pages are stored
// once per distinct content, so a table that never changes costs one copy per run.
//
// File: "PSC1", then records. 'P' page: u64 addr, u64 id, 4096 bytes. 'C' call: u64 fn (main offset), u64 lr, u64 x[31], u64 sp,
// u64 v[8][2], u64 fpcr, u64 seq, u32 npages, npages x (u64 addr, u64 id). A call refers only to pages written before it.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "loader.h"

using namespace loader;

namespace {

constexpr uint64_t kMainBase = 0x7100000000ull;
constexpr uint64_t kPage = 4096;
constexpr size_t kBudget = 8000;    // pages per snapshot

struct Root {
    int reg = 0;
    std::vector<uint64_t> loads;   // offsets to load through
    uint64_t add = 0, win = 0x1000;
    int depth = 2;
    bool map = false;
};

struct FnSpec {
    uint64_t addr = 0;
    bool triggered = false;
    int depth = 1;
    uint64_t win = 0x400;
    int max = 3000, taken = 0;
    uint64_t lr_lo = 0, lr_hi = ~0ull;
    bool armed = false;   // for 't': a plain function was entered since our last snapshot
    std::vector<Root> roots;
};

bool g_on;
FILE* g_out;
std::vector<FnSpec> g_fns;
uint64_t g_below = 0x8000, g_above = 0x2000;
uint64_t g_seq;
std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint64_t>> g_known;   // page addr -> content hash -> id
uint64_t g_next_id = 1;

uint64_t fnv(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// Readable committed memory, cached per page for one snapshot.
struct Readable {
    std::unordered_map<uint64_t, bool> cache;
    bool page(uint64_t a) {
        if (a < 0x10000 || a >= 0x800000000000ull) return false;
        uint64_t k = a & ~(kPage - 1);
        auto it = cache.find(k);
        if (it != cache.end()) return it->second;
        MEMORY_BASIC_INFORMATION mi{};
        bool ok = VirtualQuery((void*)k, &mi, sizeof mi) == sizeof mi && mi.State == MEM_COMMIT &&
                  !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (mi.Protect & 0xee);
        cache[k] = ok;
        return ok;
    }
    bool u64(uint64_t a, uint64_t& v) {
        if (!page(a) || !page(a + 7)) return false;
        v = *(const uint64_t*)a;
        return true;
    }
};

void parse_roots(const char* s) {
    // <fn hex>@x<n>[>off...][+add]:w<hex>[:d<n>][:map]   separated by ';'
    while (s && *s) {
        char* end;
        uint64_t fn = kMainBase + strtoull(s, &end, 16);
        s = end;
        Root r;
        if (*s == '@') s++;
        if (*s == 'x') { r.reg = (int)strtol(s + 1, &end, 10); s = end; }
        while (*s == '>') { r.loads.push_back(strtoull(s + 1, &end, 16)); s = end; }
        if (*s == '+') { r.add = strtoull(s + 1, &end, 16); s = end; }
        while (*s == ':') {
            s++;
            if (*s == 'w') { r.win = strtoull(s + 1, &end, 16); s = end; }
            else if (*s == 'd') { r.depth = (int)strtol(s + 1, &end, 10); s = end; }
            else if (!strncmp(s, "map", 3)) { r.map = true; s += 3; }
        }
        for (auto& f : g_fns) if (f.addr == fn) f.roots.push_back(r);
        if (*s == ';') s++;
        else break;
    }
}

void parse() {
    const char* f = getenv("SNAP_PSCAP");
    const char* s = getenv("SNAP_PSCAP_FNS");
    if (!f || !*f || !s || !*s) return;
    while (*s) {
        FnSpec fs;
        char* end;
        fs.addr = kMainBase + strtoull(s, &end, 16);
        s = end;
        if (*s == ':') {
            s++;
            while (*s && *s != ',') {
                char k = *s++;
                if (k == 't') fs.triggered = true;
                else if (k == 'd') { fs.depth = (int)strtol(s, &end, 10); s = end; }
                else if (k == 'w') { fs.win = strtoull(s, &end, 16); s = end; }
                else if (k == 'm') { fs.max = (int)strtol(s, &end, 10); s = end; }
                else if (k == 'l') {
                    fs.lr_lo = kMainBase + strtoull(s, &end, 16);
                    s = end;
                    if (*s == '-') { fs.lr_hi = kMainBase + strtoull(s + 1, &end, 16); s = end; }
                }
            }
        }
        g_fns.push_back(fs);
        if (*s == ',') s++;
    }
    parse_roots(getenv("SNAP_PSCAP_ROOTS"));
    if (const char* st = getenv("SNAP_PSCAP_STACK")) {
        char* end;
        g_below = strtoull(st, &end, 0);
        if (*end == ',') g_above = strtoull(end + 1, &end, 0);
    }
    g_out = fopen(f, "wb");
    if (!g_out) { trace("pscap: cannot open %s", f); return; }
    fwrite("PSC1", 1, 4, g_out);
    g_on = true;
    trace("pscap: %zu functions -> %s", g_fns.size(), f);
}

struct Walk {
    Readable rd;
    std::unordered_set<uint64_t> pages;
    std::unordered_map<uint64_t, int> best;   // window start -> deepest remaining depth already walked
    struct Item { uint64_t a, n, w; int d; };
    std::deque<Item> work;

    void add(uint64_t a, uint64_t n, uint64_t w, int d) { if (n) work.push_back({a, n, w, d}); }
    void copy_range(uint64_t a, uint64_t n) {
        for (uint64_t p = a & ~(kPage - 1); p < a + n; p += kPage)
            if (rd.page(p)) pages.insert(p);
    }
    void run() {
        while (!work.empty() && pages.size() < kBudget) {
            Item it = work.front();
            work.pop_front();
            uint64_t a = it.a & ~7ull, e = it.a + it.n;
            if (!rd.page(a)) continue;
            auto b = best.find(a);
            if (b != best.end() && b->second >= it.d) continue;
            best[a] = it.d;
            for (uint64_t p = a & ~(kPage - 1); p < e; p += kPage) {
                if (!rd.page(p)) { e = p > a ? p : a; break; }
                pages.insert(p);
            }
            if (it.d <= 0) continue;
            for (uint64_t q = a; q + 8 <= e; q += 8) {
                uint64_t v = *(const uint64_t*)q;
                if ((v & 3) || !rd.page(v)) continue;
                add(v, it.w, it.w, it.d - 1);
            }
        }
    }
    // libc++ unordered_map: [0] bucket array, [1] bucket count, [2] first node (singly linked, next at node+0).
    void map(uint64_t m, uint64_t node_win, int d) {
        uint64_t buckets = 0, count = 0, node = 0;
        if (!rd.u64(m, buckets) || !rd.u64(m + 8, count) || !rd.u64(m + 16, node)) return;
        copy_range(m, 24);
        if (count && count < (1u << 20)) copy_range(buckets, count * 8);
        for (int i = 0; node && i < 100000; i++) {
            copy_range(node, node_win);
            add(node, node_win, node_win, d);
            if (!rd.u64(node, node)) break;
        }
    }
};

void snapshot(FnSpec& fs, uint64_t lr) {
    Cpu* c = g_tls_cpu;
    if (!c) return;
    Walk w;
    for (auto& r : fs.roots) {
        uint64_t a = c->x[r.reg];
        bool ok = true;
        for (uint64_t off : r.loads) {
            w.copy_range(a + off, 8);
            if (!w.rd.u64(a + off, a)) { ok = false; break; }
        }
        if (!ok) continue;
        a += r.add;
        if (r.map) w.map(a, r.win, r.depth);
        else w.add(a, r.win, r.win, r.depth);
    }
    w.add(c->sp - g_below, g_below + g_above, fs.win, fs.depth);   // stack frames, the caller's locals included
    for (int i = 0; i < 31; i++) w.add(c->x[i], fs.win, fs.win, fs.depth);
    w.run();
    std::vector<std::pair<uint64_t, uint64_t>> refs;
    for (uint64_t p : w.pages) {
        const uint8_t* b = (const uint8_t*)p;
        uint64_t h = fnv(b, kPage);
        auto& m = g_known[p];
        auto f = m.find(h);
        uint64_t id;
        if (f != m.end()) id = f->second;
        else {
            id = g_next_id++;
            m[h] = id;
            fputc('P', g_out);
            fwrite(&p, 8, 1, g_out);
            fwrite(&id, 8, 1, g_out);
            fwrite(b, 1, kPage, g_out);
        }
        refs.push_back({p, id});
    }
    uint64_t fn = fs.addr - kMainBase;
    fputc('C', g_out);
    fwrite(&fn, 8, 1, g_out);
    fwrite(&lr, 8, 1, g_out);
    fwrite(c->x, 8, 31, g_out);
    fwrite(&c->sp, 8, 1, g_out);
    fwrite(c->v, 16, 8, g_out);
    fwrite(&c->fpcr, 8, 1, g_out);
    uint64_t seq = g_seq++;
    fwrite(&seq, 8, 1, g_out);
    uint32_t n = (uint32_t)refs.size();
    fwrite(&n, 4, 1, g_out);
    for (auto& r : refs) { fwrite(&r.first, 8, 1, g_out); fwrite(&r.second, 8, 1, g_out); }
    fflush(g_out);
    if (fs.taken < 4 || (fs.taken % 200) == 0)
        trace("pscap: main+%#llx #%d pages=%u (distinct pages stored %llu)", (unsigned long long)fn, fs.taken, n,
              (unsigned long long)(g_next_id - 1));
}

}  // namespace

extern "C" void snap_pscap_observe(uint64_t a, uint64_t lr, uint64_t x0) {
    (void)x0;
    static const bool parsed = (parse(), true);
    (void)parsed;
    if (!g_on) return;
    for (auto& fs : g_fns) {
        if (fs.addr != a) continue;
        if (lr < fs.lr_lo || lr >= fs.lr_hi) return;
        static std::mutex mu;   // lifted code runs on several guest threads
        std::lock_guard<std::mutex> lk(mu);
        if (fs.triggered && !fs.armed) return;
        if (fs.taken >= fs.max) return;
        if (!fs.triggered)
            for (auto& o : g_fns) if (o.triggered) o.armed = true;
        fs.armed = false;
        snapshot(fs, lr);
        fs.taken++;
        return;
    }
}
