// SPDX-License-Identifier: MIT
// Loose-file DRP entry mods: the hook the lifted DRP entry worker calls (project code, ours). Guide: docs/mods.md.
//
// Hook point: main+0x5e3a58, inside the worker at main+0x5e38b0 that decompresses one DRP entry (the only caller of
// OodleLZ_Decompress, main+0x7240, at 0x5e3b84). recomp/snap_gen.py MAIN_INLINE_HOOKS makes the generated code call
// rt_mod_entry(Cpu*) just before the instruction there. At that point (callee-saved registers are always in the Cpu):
//   x20 = the entry's chunk in memory (already decrypted): crc32 of the data, 'resd', total, ext crc, csize, dsize,
//         NUL-terminated name at +0x18, padded to 4 bytes, then the payload
//   x24 = the request object (a std::string with the entry name at +0x10)
// The worker allocates the output buffer from the chunk's dsize, then either memcpy()s the payload (csize == dsize), runs
// Oodle, and finally checks crc32(output, dsize) against the chunk's first word. A replacement therefore needs no other
// patch: the hook swaps x20 for a host-built chunk whose csize == dsize == the file size and whose crc is the file's.
// A larger file just means a larger dsize, so the game allocates what it needs.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <thread>
#include <unordered_set>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>

#include "../cpu.h"
#include "mods_core.h"

namespace ra { void report(); void note_open(const std::string& a); int before_read(const mods::Override* o, const std::string& archive); void after_read(const std::string& archive, double ms); }

namespace {

mods::Index g_index;
std::atomic<int> g_state{0};   // 0 = not loaded, 1 = no overrides, 2 = overrides present
std::once_flag g_once;
std::atomic<unsigned> g_hits{0}, g_misses{0}, g_failed{0};

struct HitCount { std::atomic<unsigned> n{0}; };
std::vector<HitCount*> g_counts;   // by index into g_labels
std::vector<const mods::Override*> g_ovr;

void summary() {
  if (g_state.load() != 2) return;
  ra::report();
  fprintf(stderr, "[mods] summary: %u replacements served, %u failed, %zu overrides installed\n",
          g_hits.load(), g_failed.load(), g_index.map.size());
  for (size_t i = 0; i < g_ovr.size(); i++)
    if (g_counts[i]->n) fprintf(stderr, "[mods]   %s <- %s x%u\n", g_ovr[i]->label.c_str(), g_index.packs[g_ovr[i]->pack].id.c_str(), g_counts[i]->n.load());
}

void init() {
  if (const char* sm = getenv("SNAP_SAFE_MODE"); sm && sm[0] == '1') {   // safe mode (docs/safe-mode.md): no mod packs, no HD packs, nothing replaced
    fprintf(stderr, "[mods] safe mode: no packs loaded\n");
    g_state = 1;
    return;
  }
  std::string root = mods::default_root();
  std::vector<std::string> warn;
  mods::load(root, g_index, warn);
  for (auto& w : warn) fprintf(stderr, "[mods] warning: %s\n", w.c_str());
  for (auto& c : g_index.conflicts)
    fprintf(stderr, "[mods] conflict: %s: %s beats %s\n", c.path.c_str(), g_index.packs[c.winner].id.c_str(), g_index.packs[c.loser].id.c_str());
  for (auto& l : g_index.rom_layers) fprintf(stderr, "[mods] rom layer %s <- %s\n", l.first.c_str(), g_index.packs[l.second].id.c_str());
  if (g_index.empty()) {
    if (!g_index.packs.empty()) fprintf(stderr, "[mods] root %s: %zu packs, no overrides\n", root.c_str(), g_index.packs.size());
    g_state = 1;
    return;
  }
  fprintf(stderr, "[mods] root %s: %zu overrides\n", root.c_str(), g_index.map.size());
  for (auto& kv : g_index.map) {
    g_ovr.push_back(&kv.second);
    g_counts.push_back(new HitCount());
    fprintf(stderr, "[mods] %s <- %s\n", kv.second.label.c_str(), g_index.packs[kv.second.pack].id.c_str());
  }
  atexit(summary);
  g_state = 2;
}

std::vector<uint8_t>& scratch() { static thread_local std::vector<uint8_t> v; return v; }

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz;
  bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart < (1ll << 31);
  if (ok) {
    out.resize((size_t)sz.QuadPart);
    size_t done = 0;
    while (ok && done < out.size()) {
      DWORD got = 0;
      ok = ReadFile(h, out.data() + done, (DWORD)std::min<size_t>(out.size() - done, 1u << 24), &got, nullptr) && got;
      done += got;
    }
  }
  CloseHandle(h);
  return ok;
}

// Structural check of a replacement nutexb (ext id 5C156DBC) so a malformed file falls back to the original instead of
// reaching the renderer. Footer (last 0xb0 bytes): u32 w,h,depth,format,dim,mips,tile,layers,data_size,"XET ",version at
// footer+0x84. Returns a reason string, or nullptr when it looks sane. Sizes are NOT compared with the original (any size is fine).
const char* nutexb_problem(const std::vector<uint8_t>& d) {
  if (d.size() < 0xb0 + 16) return "smaller than a nutexb footer";
  const uint8_t* f = d.data() + d.size() - 0xb0;
  uint32_t v[11];
  std::memcpy(v, f + 0x84, sizeof v);
  if (v[9] != 0x54455820u) return "no XET footer magic";
  uint32_t w = v[0], h = v[1], depth = v[2], dim = v[4], mips = v[5], layers = v[7], dsz = v[8];
  if (!w || !h || w > 16384 || h > 16384) return "bad width or height";
  if (!depth || depth > 2048 || !layers || layers > 64) return "bad depth or layer count";
  if (dim != 2 && dim != 3 && dim != 4) return "bad dimension";
  if (!mips || mips > 15) return "bad mip count";
  if ((w >> (mips - 1)) == 0 && (h >> (mips - 1)) == 0) return "more mips than the size allows";
  if ((uint64_t)dsz + 0xb0 > d.size()) return "data size larger than the file";
  uint64_t sum = 0;
  uint32_t ms[16];
  std::memcpy(ms, f, sizeof ms);
  for (uint32_t i = 0; i < mips; i++) sum += ms[i];
  if (sum == 0 || sum > dsz) return "mip sizes do not fit the data size";
  return nullptr;
}

}  // namespace

// ---- SNAP_MODS_AB=<P> (test aid, off when unset): in-run before/after of replaced textures. While (present count / P) is
// odd the renderer shows the ORIGINAL texture, otherwise the replacement. SNAP_MODS_AB=key: F8 switches instead. Each replaced nutexb is paired with the original
// file from SNAP_MODS_AB_ORIG (default extracted/drp-v262144, same relative path as in the pack); the renderer
// (runtime/nvn/nvn_draw.cpp ResolveHandle) finds the pair by comparing the guest texture's bytes with the replacement and
// samples a second image built from the original at bind time. Both versions stay resident; nothing is re-uploaded.
namespace {
// Only the first `cmp` bytes of the replacement are kept (2026-11-21): holding the whole file kept a second copy of every
// replacement next to the loader's built chunk, about 2 GB more with an 8 GB pack, and the hidden test runs' 14 GB job cap
// then refused commits (error 1455) and the game aborted (abort, 0xC0000409) mid-ride.
struct AbEntry {
  std::vector<uint8_t> repl;   // the replacement's first `cmp` data bytes
  std::shared_ptr<const std::vector<uint8_t>> orig;
  uint32_t rw, rh, rm, ow, oh, om;   // replacement and original width, height, mip count
  size_t cmp;                        // bytes of the replacement data compared with the guest's copy
};
std::mutex g_ab_mu;
std::vector<AbEntry> g_ab;

bool ab_footer(const std::vector<uint8_t>& d, uint32_t* w, uint32_t* h, uint32_t* mips, uint32_t* fmt, uint32_t* dsz) {
  if (d.size() < 0xb0 + 16) return false;
  uint32_t v[11];
  std::memcpy(v, d.data() + d.size() - 0xb0 + 0x84, sizeof v);
  if (v[9] != 0x54455820u) return false;
  *w = v[0]; *h = v[1]; *fmt = v[3]; *mips = v[5]; *dsz = v[8];
  return true;
}

void ab_register(const mods::Override* o, const std::shared_ptr<const std::vector<uint8_t>>& repl) {
  static const char* e = getenv("SNAP_MODS_AB_ORIG");
  const std::string root = e && *e ? e : "extracted/drp-v262144";
  const std::string& dir = g_index.packs[o->pack].dir;
  std::string f = o->file;
  for (auto& ch : f) if (ch == '\\') ch = '/';
  std::string d = dir;
  for (auto& ch : d) if (ch == '\\') ch = '/';
  if (f.size() <= d.size() + 1 || f.compare(0, d.size(), d) != 0) return;
  auto orig = std::make_shared<std::vector<uint8_t>>();
  if (!read_file(root + "/" + f.substr(d.size() + 1), *orig)) { fprintf(stderr, "[mods-ab] no original for %s under %s\n", o->label.c_str(), root.c_str()); return; }
  uint32_t rw, rh, rm, rf, rd, ow, oh, om, of, od;
  if (!ab_footer(*repl, &rw, &rh, &rm, &rf, &rd) || !ab_footer(*orig, &ow, &oh, &om, &of, &od) || rf != of) return;
  if (rd == 0 || rd > repl->size() - 0xb0) return;   // a bad footer data size would read past the file (or match any texture of the same size)
  const size_t cmp = std::min<size_t>(rd, 4096);
  std::lock_guard<std::mutex> g(g_ab_mu);
  g_ab.push_back(AbEntry{std::vector<uint8_t>(repl->begin(), repl->begin() + cmp), orig, rw, rh, rm, ow, oh, om, cmp});
  if (getenv("SNAP_MODS_AB_LOG")) fprintf(stderr, "[mods-ab] registered %s: replacement %ux%u/%u, original %ux%u/%u\n", o->label.c_str(), rw, rh, rm, ow, oh, om);
}
}  // namespace

// Renderer side: the period (0 = off). SNAP_MODS_AB=key pairs textures the same way (period 1) but F8 picks the side.
static bool ab_keyed() {
  static const bool k = getenv("SNAP_MODS_AB") && _stricmp(getenv("SNAP_MODS_AB"), "key") == 0;
  return k;
}
extern "C" unsigned snap_mods_ab_period() {
  static const unsigned p = ab_keyed() ? 1u : getenv("SNAP_MODS_AB") ? (unsigned)strtoul(getenv("SNAP_MODS_AB"), nullptr, 0) : 0;
  return p;
}
// Renderer side, once per bind: true while the ORIGINAL should show. Keyed mode: F8 toggles (polled once per present, so
// the cost is one compare per bind); starts on the pack. Otherwise odd (present / period) shows the original.
extern "C" bool snap_mods_ab_live(unsigned present) {
  static const unsigned P = snap_mods_ab_period();
  if (!P) return false;
  if (!ab_keyed()) return (present / P) & 1;
  static std::atomic<unsigned> seen{~0u};
  static std::atomic<bool> orig{false}, down{false};
  if (seen.exchange(present, std::memory_order_relaxed) != present) {
    const bool d = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (d && !down.exchange(true)) {
      const bool o = !orig.load();
      orig.store(o);
      fprintf(stderr, "[mods-ab] F8: showing %s textures\n", o ? "original" : "pack");
    } else if (!d) {
      down.store(false);
    }
  }
  return orig.load(std::memory_order_relaxed);
}
// Renderer side: `src` is the guest's copy of a texture of w x h x mips. Returns the original's data and size, or null.
extern "C" const uint8_t* snap_mods_ab_original(const uint8_t* src, uint32_t w, uint32_t h, uint32_t mips,
                                                 uint32_t* ow, uint32_t* oh, uint32_t* om) {
  std::lock_guard<std::mutex> g(g_ab_mu);
  static const bool dbg = getenv("SNAP_MODS_AB_LOG") != nullptr;
  for (auto& e : g_ab) {
    if (e.rw == w && e.rh == h && e.rm == mips && dbg) {
      static int n = 0;
      if (e.rw >= 4096 && n++ < 40) {
        size_t k = 0;
        while (k < e.cmp && e.repl.data()[k] == src[k]) k++;
        fprintf(stderr, "[mods-ab] size match %ux%u/%u: first difference at byte %zu of %zu (repl %02x guest %02x, guest head %02x%02x%02x%02x, repl head %02x%02x%02x%02x)\n", w, h, mips, k, e.cmp,
                k < e.cmp ? e.repl.data()[k] : 0, k < e.cmp ? src[k] : 0, src[0], src[1], src[2], src[3], e.repl[0], e.repl[1], e.repl[2], e.repl[3]);
      }
    }
    if (e.rw != w || e.rh != h || e.rm != mips || std::memcmp(e.repl.data(), src, e.cmp) != 0) continue;
    *ow = e.ow; *oh = e.oh; *om = e.om;
    return e.orig->data();
  }
  return nullptr;
}

// Archive path (normalized, e.g. "common/ui/master/foo.drp") of the entry worker's current chunk; false when unknown.
bool mods_archive_of(Cpu* c, std::string* out);

static std::mutex g_file_mu;
static std::unordered_map<const mods::Override*, std::shared_ptr<const std::vector<uint8_t>>> g_files;

// [perf] Built-chunk cache (perf pass 4, 2026-11-13): the first hit of an override builds the replacement chunk (header +
// payload + crc32) once; later hits whose original header is the same hand the guest that shared, read-only chunk (the worker
// only reads it: memcpy or Oodle, then its own crc32 of the output). Before, every hit allocated, copied and crc32'd up to 22 MB
// per entry inside the game's loader (which holds a guest lock meanwhile), so loader threads queued on it. Built chunks are
// never freed (one per override). SNAP_MODS_CHUNK_CACHE=0 restores the per-hit build.
namespace {
struct BuiltChunk { std::vector<uint8_t> v; size_t hdr; };
std::mutex g_built_mu;
std::unordered_map<const mods::Override*, const BuiltChunk*> g_built;
bool chunk_cache_on() {
  static const bool on = [] { const char* e = getenv("SNAP_MODS_CHUNK_CACHE"); return !(e && *e == '0'); }();
  return on;
}
// The original header bytes that the replacement keeps (resd, ext crc, name); the others are rewritten by build_chunk.
bool same_header(const BuiltChunk* b, const uint8_t* chunk, size_t hdr) {
  return b->hdr == hdr && std::memcmp(b->v.data() + 0x04, chunk + 0x04, 4) == 0 && std::memcmp(b->v.data() + 0x0c, chunk + 0x0c, 4) == 0 &&
         std::memcmp(b->v.data() + 0x18, chunk + 0x18, hdr - 0x18) == 0;
}
}  // namespace

// ---- Course read-ahead (SNAP_MODS_READAHEAD, setting mods_readahead, --mods-readahead; docs/mods.md "Read-ahead").
// The game opens a course's pack file (nx/Pack/LevelComposite/master/LC_<id>_<x>.drp, then its characters and UI packs) and then
// loads every entry of it in a burst. The first load of each replaced texture reads the file from disk on the loader thread
// (read_file below), so with a big pack the first ride of a session waits on the disk. When the game opens a .drp that has
// replacements, a low-priority thread reads those files once into the Windows file cache, so the loader's own read is a memory
// copy. Bounded: at most SNAP_MODS_READAHEAD_MB (default 1536) of files read but not yet loaded by the game, and opening a
// different LC_ course cancels the unread rest of the old one (queued and running). A wrong guess only wastes reads; nothing
// here changes what the game receives, and the loader never waits for it.
namespace ra {
constexpr bool kDefaultOn = true;   // measured 2026-11-23, docs/mods.md "Read-ahead"
bool enabled() {
  static const bool on = [] { const char* e = getenv("SNAP_MODS_READAHEAD"); return e && *e ? *e != '0' : kDefaultOn; }();
  return on;
}
struct St { int st = 0; int64_t sz = 0; };   // 1 reading, 2 in the cache, 3 loaded by the game
struct ArcStat { double open = -1, first = -1, last = -1; unsigned hits = 0, ready = 0, racing = 0, late = 0; double wait_ms = 0; double pf_start = -1, pf_end = -1; unsigned pf_files = 0, pf_skip = 0, shown_hits = 0, shown_pf = 0; };
std::mutex mu;
std::condition_variable cv;
std::unordered_map<std::string, std::vector<const mods::Override*>> by_archive;
bool indexed = false, started = false;
std::deque<std::string> queue;
std::unordered_set<std::string> seen;
std::unordered_map<const mods::Override*, St> state;
std::map<std::string, ArcStat> arcs;
std::string cur_group;
std::atomic<uint64_t> epoch{0};
int64_t ahead = 0;                      // bytes read ahead and not yet loaded by the game (under mu)
int64_t peak_ahead = 0;
std::atomic<bool> dirty{false};
void reporter();
std::atomic<uint64_t> n_files{0}, n_bytes{0}, n_cancel{0}, n_skip{0}, n_wait_files{0}, n_wait_us{0}, n_slow{0};

// Seconds since process start: the same clock as the "[  36.2657]" stamps of the run log, within a few ms.
double uptime() {
  static FILETIME ct = [] { FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u); return c; }();
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER a, b;
  a.LowPart = ct.dwLowDateTime; a.HighPart = ct.dwHighDateTime; b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
  return (double)(b.QuadPart - a.QuadPart) / 1e7;
}
// "nx/pack/levelcomposite/master/lc_001_a.drp" -> "lc_001"; other packs: ""
std::string group_of(const std::string& a) {
  size_t p = a.find("levelcomposite/master/lc_");
  if (p == std::string::npos) return "";
  p += strlen("levelcomposite/master/");
  if (atoi(a.c_str() + p + 3) >= 100) return "";   // the lab and hub packs (LC_101 and up) belong to no course: never cancelled
  size_t q = a.find('_', p + 3);
  return a.substr(p, q == std::string::npos ? std::string::npos : q - p);
}
int64_t budget() {
  static const int64_t b = [] { const char* e = getenv("SNAP_MODS_READAHEAD_MB"); long v = e ? atol(e) : 1536; return (int64_t)(v < 16 ? 16 : v) << 20; }();
  return b;
}

// Reads the file through the cache. Returns bytes read, 0 when stopped early or unreadable.
int64_t read_through(const std::string& path, const std::function<bool()>& stop) {
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return 0;
  static thread_local std::vector<uint8_t> buf(1 << 20);
  int64_t total = 0;
  for (;;) {
    if (stop()) { total = 0; break; }
    DWORD got = 0;
    if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr) || !got) break;
    total += got;
  }
  CloseHandle(h);
  return total;
}

void worker() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
  for (;;) {
    std::string arc;
    uint64_t my_epoch;
    std::vector<const mods::Override*> files;
    {
      std::unique_lock<std::mutex> g(mu);
      cv.wait(g, [] { return !queue.empty(); });
      arc = queue.front();
      queue.pop_front();
      my_epoch = epoch.load();
      files = by_archive[arc];
      arcs[arc].pf_start = uptime();
    }
    const std::string grp = group_of(arc);
    auto cancelled = [&] { return !grp.empty() && epoch.load() != my_epoch; };
    for (const mods::Override* o : files) {
      if (cancelled()) { n_cancel++; break; }
      int64_t sz = 0;
      {   // wait for budget room (the game loading what we read frees it); skip files the game already loaded or another worker has
        std::unique_lock<std::mutex> g(mu);
        if (state.find(o) != state.end()) { n_skip++; arcs[arc].pf_skip++; continue; }
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExA(o->file.c_str(), GetFileExInfoStandard, &fa)) continue;
        sz = ((int64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        while (ahead > 0 && ahead + sz > budget() && !cancelled()) cv.wait_for(g, std::chrono::milliseconds(20));
        if (cancelled()) { n_cancel++; break; }
        if (state.find(o) != state.end()) { n_skip++; continue; }
        state[o] = St{1, sz};
        ahead += sz;
        if (ahead > peak_ahead) peak_ahead = ahead;
      }
      const int64_t got = read_through(o->file, cancelled);
      {
        std::lock_guard<std::mutex> g(mu);
        auto it = state.find(o);
        if (it != state.end() && it->second.st == 1) {
          if (got > 0) { it->second.st = 2; n_files++; n_bytes += (uint64_t)got; arcs[arc].pf_files++; arcs[arc].pf_end = uptime(); }
          else { ahead -= it->second.sz; state.erase(it); }   // stopped or unreadable: leave no trace
        }
      }
    }
  }
}

void ensure_index_locked() {
  if (indexed) return;
  indexed = true;
  for (const mods::Override* o : g_ovr) {
    size_t p = o->label.find(".drp/");
    if (p != std::string::npos) by_archive[o->label.substr(0, p + 4)].push_back(o);
  }
}

// Ride courses are LC_001 .. LC_099 (the lab and hub packs are LC_101 and up). The player's recent course packs are kept in
// <mods root>\.readahead.txt, one normalized archive name per line, most recent first (at most 4; names of the player's own
// packs only, no game data). When the course-select scene opens, the most recent one is read ahead before the player has chosen:
// a wrong guess costs reads and is cancelled by the pack the game really opens.
std::vector<std::string> history;
std::vector<std::string> startup;   // pack files with replacements that the game opened in its first two minutes (the menus), in order, at most 100
std::string hist_path;
bool ride_course(const std::string& a) {
  const size_t p = a.find("levelcomposite/master/lc_");
  return p != std::string::npos && atoi(a.c_str() + p + strlen("levelcomposite/master/lc_")) < 100;
}
void load_history() {
  hist_path = mods::default_root() + "/.readahead.txt";
  if (FILE* f = fopen(hist_path.c_str(), "rb")) {
    char line[512];
    while (fgets(line, sizeof line, f)) {
      std::string s = line;
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
      const bool u = s.size() > 2 && s[0] == 'u' && s[1] == ' ';
      if (s.size() > 2 && s[1] == ' ' && (s[0] == 'u' || s[0] == 'c')) s = s.substr(2);
      if (u) { if (startup.size() < 100) startup.push_back(s); }
      else if (ride_course(s) && history.size() < 4) history.push_back(s);
    }
    fclose(f);
  }
}
void save_history() {
  if (FILE* f = fopen(hist_path.c_str(), "wb")) {
    for (auto& s : history) fprintf(f, "c %s\n", s.c_str());
    for (auto& s : startup) fprintf(f, "u %s\n", s.c_str());
    fclose(f);
  }
}

// Queue archive `a` (has replacements). Called with mu held.
void enqueue_locked(const std::string& a) {
  const std::string grp = group_of(a);
  if (!grp.empty() && grp != cur_group) {   // another course: drop the old course's unread files
    if (!cur_group.empty()) {
      epoch++;
      for (auto q = queue.begin(); q != queue.end();) { if (!group_of(*q).empty()) q = queue.erase(q); else ++q; }
      for (auto s = seen.begin(); s != seen.end();) { if (!group_of(*s).empty()) s = seen.erase(s); else ++s; }
      for (auto st = state.begin(); st != state.end();) {   // forget the old course's read-ahead that the game has not loaded, so its budget is free
        if (st->second.st != 3 && !group_of(st->first->label).empty()) { ahead -= st->second.sz; st = state.erase(st); } else ++st;
      }
      if (ahead < 0) ahead = 0;
    }
    cur_group = grp;
  }
  if (!seen.insert(a).second) return;
  queue.push_back(a);
  if (!started) {
    started = true;
    static const int n = [] { const char* e = getenv("SNAP_MODS_READAHEAD_THREADS"); int v = e ? atoi(e) : 2; return v < 1 ? 1 : v > 8 ? 8 : v; }();
    for (int i = 0; i < n; i++) std::thread(worker).detach();
  }
  cv.notify_all();
}

// The game opened archive `a` (normalized).
void note_open(const std::string& a) {
  std::lock_guard<std::mutex> g(mu);
  ArcStat& as = arcs[a];
  if (as.open < 0) as.open = uptime();
  if (!enabled()) return;
  if (!indexed) {
    ensure_index_locked();
    load_history();
    for (const std::string& s : startup) if (by_archive.count(s)) enqueue_locked(s);   // the menus' packs, before the game gets to them
  }
  if (!ride_course(a) && uptime() < 120 && startup.size() < 100 && by_archive.count(a) && std::find(startup.begin(), startup.end(), a) == startup.end()) {
    startup.push_back(a);
    save_history();
  }
  if (ride_course(a) && (history.empty() || history[0] != a)) {
    history.erase(std::remove(history.begin(), history.end(), a), history.end());
    history.insert(history.begin(), a);
    if (history.size() > 4) history.resize(4);
    save_history();
  }
  if (by_archive.find(a) != by_archive.end()) enqueue_locked(a);
  if (a.size() > 21 && a.compare(a.size() - 21, 21, "scenes/map_select.drp") == 0 && !history.empty() && by_archive.count(history[0])) {
    if (const char* p = getenv("SNAP_MODS_READAHEAD_GUESS"); !(p && *p == '0')) enqueue_locked(history[0]);
  }
}

// The loader is about to read `o` from disk for the first time (called with or without read-ahead, for the report).
int before_read(const mods::Override* o, const std::string& archive) {
  static std::once_flag rep_once;
  std::call_once(rep_once, [] { std::thread(reporter).detach(); });
  dirty = true;
  std::lock_guard<std::mutex> g(mu);
  int st = 0;
  auto it = state.find(o);
  if (it != state.end()) {
    st = it->second.st;
    if (st == 3) return 3;   // loaded before (the chunk header differed): not a first load
    if (st != 3) { ahead -= it->second.sz; it->second.st = 3; cv.notify_all(); }
  } else {
    state[o] = St{3, 0};
  }
  ArcStat& s = arcs[archive];
  const double now = uptime();
  if (s.first < 0) s.first = now;
  s.last = now;
  s.hits++;
  if (st == 2) s.ready++; else if (st == 1) s.racing++; else s.late++;
  return st;
}
void after_read(const std::string& archive, double ms) {
  std::lock_guard<std::mutex> g(mu);
  arcs[archive].wait_ms += ms;
  n_wait_files++;
  n_wait_us += (uint64_t)(ms * 1000);
  if (ms > 50) n_slow++;
}
// Test runs end the process without running atexit handlers, so the totals are also printed every 3 s while they change.
void reporter() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(3));
    if (dirty.exchange(false)) report();
  }
}
void report() {
  std::lock_guard<std::mutex> g(mu);
  unsigned ready = 0, racing = 0, late = 0;
  for (auto& kv : arcs) { ready += kv.second.ready; racing += kv.second.racing; late += kv.second.late; }
  fprintf(stderr, "[mods] readahead %s: loader read %llu files, wait %.0f ms total (%llu reads over 50 ms); in cache when loaded %u, still reading %u, not started %u; read ahead %llu files %.0f MiB, skipped %llu, cancelled %llu, peak unread %.0f MiB\n",
          enabled() ? "on" : "off", (unsigned long long)n_wait_files.load(), n_wait_us.load() / 1000.0, (unsigned long long)n_slow.load(), ready, racing, late,
          (unsigned long long)n_files.load(), n_bytes.load() / 1048576.0, (unsigned long long)n_skip.load(), (unsigned long long)n_cancel.load(), peak_ahead / 1048576.0);
  for (auto& kv : arcs) {   // only the pack files that changed since the last print
    ArcStat& s = kv.second;
    if (!s.hits || (s.hits == s.shown_hits && s.pf_files == s.shown_pf)) continue;
    s.shown_hits = s.hits;
    s.shown_pf = s.pf_files;
    fprintf(stderr, "[mods]   %s: opened %.2f s, first load %.2f s, last %.2f s, %u files (%u ready, %u racing, %u late), loader read wait %.0f ms; read ahead %u files (%u already loaded) from %.2f to %.2f s\n", kv.first.c_str(), s.open, s.first, s.last, s.hits, s.ready, s.racing, s.late, s.wait_ms, s.pf_files, s.pf_skip, s.pf_start, s.pf_end);
  }
}
}  // namespace ra

// Texture capture (SNAP_TEXDUMP, mods_core.h texdump_*): host/mods/texdump.cpp sets this; it saves the original entry
// before any replacement is swapped in. g_texdump_on is fixed at start-up, so the off path costs one load.
void (*g_snap_texdump)(Cpu* c, const std::string& archive) = nullptr;
static const bool g_texdump_on = mods::texdump_enabled();

extern "C" void rt_mod_entry(Cpu* c) {
  if (g_texdump_on && g_snap_texdump && c->x[20]) {
    std::string a;
    if (mods_archive_of(c, &a)) g_snap_texdump(c, a);
  }
  int st = g_state.load(std::memory_order_acquire);
  if (st == 1) return;
  if (st == 0) { std::call_once(g_once, init); st = g_state.load(); if (st == 1) return; }
  if (g_index.empty()) return;
  const uint8_t* chunk = (const uint8_t*)c->x[20];
  if (!chunk) return;
  std::string archive;
  if (!mods_archive_of(c, &archive)) { if (g_misses++ < 5) fprintf(stderr, "[mods] archive of chunk %s unknown (no recorded read covers it)\n", (const char*)chunk + 0x18); return; }
  const char* name = (const char*)chunk + 0x18;
  size_t nlen = strnlen(name, 256);
  uint32_t extcrc;
  std::memcpy(&extcrc, chunk + 0x0c, 4);
  static const bool trace = getenv("SNAP_MODS_TRACE") != nullptr;   // log every entry the worker sees: archive/name.extcrc
  if (trace) fprintf(stderr, "[mods] entry %s/%s.%08x\n", archive.c_str(), name, extcrc);
  const mods::Override* o = g_index.find(archive, std::string(name, nlen), extcrc);
  if (!o) return;
  auto& buf = scratch();
  const size_t hdr_len = mods::chunk_header_len(nlen);
  auto count_hit = [&](size_t bytes) {
    size_t idx = 0;
    for (; idx < g_ovr.size(); idx++) if (g_ovr[idx] == o) break;
    if (idx < g_ovr.size() && g_counts[idx]->n++ == 0)
      fprintf(stderr, "[mods] hit %s <- %s (%zu bytes)\n", o->label.c_str(), g_index.packs[o->pack].id.c_str(), bytes);
    g_hits++;
    static const int delay_ms = getenv("SNAP_MODS_DELAY_MS") ? atoi(getenv("SNAP_MODS_DELAY_MS")) : 0;   // test aid: make each hit as slow as the old per-hit disk read
    if (delay_ms > 0) Sleep((DWORD)delay_ms);
  };
  if (chunk_cache_on()) {   // [perf] fast path: a chunk built by an earlier hit
    const BuiltChunk* b = nullptr;
    { std::lock_guard<std::mutex> g(g_built_mu); auto it = g_built.find(o); if (it != g_built.end()) b = it->second; }
    if (b && same_header(b, chunk, hdr_len)) {
      count_hit(b->v.size() - b->hdr);
      c->x[20] = (uint64_t)(uintptr_t)b->v.data();   // shared and never freed
      return;
    }
  }
  // Replacement files are read and validated once, then shared: the game reloads the same entries over and over, and a
  // disk read per hit (up to 22 MB each) made loading about 15x slower with a large mod set.
  std::shared_ptr<const std::vector<uint8_t>> sp;
  {
    std::lock_guard<std::mutex> g(g_file_mu);
    auto f = g_files.find(o);
    if (f != g_files.end()) sp = f->second;
  }
  if (!sp) {
    auto fresh = std::make_shared<std::vector<uint8_t>>();
    const int ra_st = ra::before_read(o, archive);
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceCounter(&q0);
    const bool read_ok = read_file(o->file, *fresh);
    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&qf);
    if (ra_st != 3) ra::after_read(archive, (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart);
    if (!read_ok) {
      if (g_failed++ < 20) fprintf(stderr, "[mods] cannot read %s (%s): using the original\n", o->file.c_str(), o->label.c_str());
      return;
    }
    if (extcrc == 0x5c156dbcu) {
      if (const char* why = nutexb_problem(*fresh)) {
        if (g_failed++ < 20) fprintf(stderr, "[mods] skipped %s <- %s: invalid nutexb (%s), using the original\n", o->label.c_str(), g_index.packs[o->pack].id.c_str(), why);
        fresh->clear();   // cache the rejection (empty) so it is not re-read
      }
    }
    sp = fresh;
    if (!fresh->empty() && extcrc == 0x5c156dbcu && snap_mods_ab_period()) ab_register(o, sp);
    std::lock_guard<std::mutex> g(g_file_mu);
    g_files.emplace(o, sp);
  }
  if (sp->empty()) return;
  const std::vector<uint8_t>& data = *sp;
  if (chunk_cache_on()) {   // [perf] first hit (or a different original header): build once and publish if this override has none yet
    auto* nb = new BuiltChunk{mods::build_chunk(chunk, hdr_len, data.data(), data.size()), hdr_len};
    const BuiltChunk* use = nb;
    {
      std::lock_guard<std::mutex> g(g_built_mu);
      auto it = g_built.find(o);
      if (it == g_built.end()) g_built.emplace(o, nb);
      else if (same_header(it->second, chunk, hdr_len)) { use = it->second; }   // another thread won the race
      else { buf = std::move(nb->v); delete nb; nb = nullptr; use = nullptr; }  // header differs from the published one: private copy
    }
    if (use != nb && nb) delete nb;
    if (use) {
      { std::lock_guard<std::mutex> g(g_file_mu); auto f = g_files.find(o); if (f != g_files.end() && !f->second->empty()) g_files.erase(f); }   // the chunk holds the data now
      count_hit(data.size());
      c->x[20] = (uint64_t)(uintptr_t)use->v.data();
      return;
    }
    count_hit(data.size());
    c->x[20] = (uint64_t)(uintptr_t)buf.data();
    return;
  }
  buf = mods::build_chunk(chunk, hdr_len, data.data(), data.size());
  count_hit(data.size());
  c->x[20] = (uint64_t)(uintptr_t)buf.data();   // held until this thread's next replacement: the worker is done with it by then
}

// ---- which archive is a chunk from? The request object carries only the entry name, so the host remembers, per read
// buffer, the DRP the game read into it (nn::fs::ReadFile taps in runtime/hle/fs.cpp). A chunk pointer lies inside the
// buffer of the read that loaded it; a later read into the same memory replaces the older record.
extern void (*g_snap_fs_tap_open)(uint64_t id, const char* path);
extern void (*g_snap_fs_tap_close)(uint64_t id);
extern void (*g_snap_fs_tap_read)(uint64_t id, const void* buf, size_t n);
extern std::vector<std::string> (*g_snap_fs_extra_rom_layers)();   // runtime/hle/fs_core.cpp, read by MountRom

namespace {
std::mutex g_tap_mu;
std::map<uint64_t, std::string> g_handles;                           // file handle -> normalized .drp path
struct Range { uint64_t end; std::string archive; };
std::map<uint64_t, Range> g_ranges;                                  // buffer start -> record

void tap_open(uint64_t id, const char* path) {
  std::string a = mods::norm_archive(path);
  if (a.size() < 4 || a.compare(a.size() - 4, 4, ".drp") != 0) return;
  { std::lock_guard<std::mutex> g(g_tap_mu); g_handles[id] = a; }
  ra::note_open(a);
}
void tap_close(uint64_t id) {
  std::lock_guard<std::mutex> g(g_tap_mu);
  g_handles.erase(id);
}
void tap_read(uint64_t id, const void* buf, size_t n) {
  std::lock_guard<std::mutex> g(g_tap_mu);
  auto h = g_handles.find(id);
  if (h == g_handles.end() || !n) return;
  uint64_t b = (uint64_t)(uintptr_t)buf, e = b + n;
  auto it = g_ranges.lower_bound(b);
  if (it != g_ranges.begin()) { auto p = std::prev(it); if (p->second.end > b) it = p; }
  while (it != g_ranges.end() && it->first < e) it = g_ranges.erase(it);   // overlapped records are stale
  g_ranges[b] = Range{e, h->second};
}

// The pack index loads on the first game file access, never before main(): --data-dir, portable.txt and --safe-mode are applied in
// main() (host/boot.cpp), and a load in this static initializer read the mods folder of the data folder that was current before them.
bool taps_live() {
  static std::once_flag said;
  std::call_once(g_once, init);
  if (g_texdump_on) std::call_once(said, [] { fprintf(stderr, "[texdump] capture on: %s\n", mods::texdump_root().c_str()); });
  return g_state.load() == 2 || g_texdump_on;
}
void lazy_tap_open(uint64_t id, const char* path) { if (taps_live()) tap_open(id, path); }
void lazy_tap_close(uint64_t id) { if (taps_live()) tap_close(id); }
void lazy_tap_read(uint64_t id, const void* buf, size_t n) { if (taps_live()) tap_read(id, buf, n); }

struct Installer {
  Installer() {
    g_snap_fs_tap_open = lazy_tap_open; g_snap_fs_tap_close = lazy_tap_close; g_snap_fs_tap_read = lazy_tap_read;
    g_snap_fs_extra_rom_layers = [] { std::call_once(g_once, init); std::vector<std::string> v; for (auto& l : g_index.rom_layers) v.push_back(l.first); return v; };
  }
} g_installer;
}  // namespace

bool mods_archive_of(Cpu* c, std::string* out) {
  uint64_t a = c->x[20];
  std::lock_guard<std::mutex> g(g_tap_mu);
  auto it = g_ranges.upper_bound(a);
  if (it == g_ranges.begin()) return false;
  --it;
  if (a >= it->second.end) return false;
  *out = it->second.archive;
  return true;
}
