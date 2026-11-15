// SPDX-License-Identifier: MIT
// Save import and export (see save_port.h).
#include "save_port.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <tuple>

#include "hle_config.h"
#include "save_gen.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace hle {
namespace saveport {

namespace fs = std::filesystem;
namespace sg = hle::savegen;

static fs::path P(const std::string& s) { return fs::u8path(s); }
static std::string S(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }
static std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

static const std::vector<FileSpec>* g_test_table = nullptr;
void set_expected_files_for_test(const std::vector<FileSpec>* v) { g_test_table = v; }

const std::vector<FileSpec>& expected_files() {
  if (g_test_table) return *g_test_table;
  static const std::vector<FileSpec> v = {
      {"00c31e53dde3ac51", 439664304, 439664304}, {"04289da492638279", 912432, 460848},
      {"0a980178a54e26f3", 352, 352},             {"398ecf2b56c614e3", 192, 192},
      {"4207cfe275441231", 12176, 12176},         {"4b1a0e859a3f7816", 15568, 15568},
      {"4f342de95be7450e", 10338096, 10338096},   {"5652712535912f19", 192, 192},
      {"6706e7d0f3dab2ab", 240496, 240496},       {"75006a27cf0c9275", 19248, 19248},
      {"76401e93e6565bd5", 3056, 3056},           {"9661edcf41d19929", 36879408, 36879408},
      {"9c1c815c54797c18", 1095012912, 553036848}, {"b8b0ce894762e7a8", 73142832, 36940848},
      {"bf0b4b223f011bf9", 256, 256},
  };
  return v;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Inflate (RFC 1951), streaming, no dependencies (the same canonical-Huffman method as zlib's puff).
namespace {

uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t n) {
  static uint32_t tab[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; }
    init = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

struct BitIn {
  std::ifstream& f;
  uint64_t remaining;   // compressed bytes left
  uint8_t buf[1 << 16];
  size_t pos = 0, len = 0;
  uint32_t bitbuf = 0;
  int bitcnt = 0;
  bool err = false;
  BitIn(std::ifstream& s, uint64_t n) : f(s), remaining(n) {}
  int byte() {
    if (pos == len) {
      size_t want = (size_t)std::min<uint64_t>(remaining, sizeof buf);
      if (!want) { err = true; return 0; }
      f.read((char*)buf, (std::streamsize)want);
      len = (size_t)f.gcount();
      remaining -= len;
      pos = 0;
      if (!len) { err = true; return 0; }
    }
    return buf[pos++];
  }
  uint32_t bits(int n) {
    while (bitcnt < n) { bitbuf |= (uint32_t)byte() << bitcnt; bitcnt += 8; }
    uint32_t v = bitbuf & ((1u << n) - 1);
    bitbuf >>= n;
    bitcnt -= n;
    return v;
  }
};

struct OutWin {
  std::ofstream& f;
  uint8_t ring[1 << 16];
  uint64_t total = 0, flushed = 0;
  uint32_t crc = 0;
  bool err = false;
  explicit OutWin(std::ofstream& s) : f(s) {}
  void put(uint8_t b) {
    ring[total & 0xFFFF] = b;
    ++total;
    if (total - flushed == 0x8000) drain();
  }
  void drain() {
    size_t n = (size_t)(total - flushed);
    if (!n) return;
    const uint8_t* p = ring + (flushed & 0xFFFF);
    f.write((const char*)p, (std::streamsize)n);
    if (!f) err = true;
    crc = crc32_update(crc, p, n);
    flushed = total;
  }
};

struct Huff { uint16_t count[16]; uint16_t symbol[320]; };

int huff_build(Huff& h, const uint8_t* len, int n) {
  for (int i = 0; i < 16; ++i) h.count[i] = 0;
  for (int s = 0; s < n; ++s) h.count[len[s]]++;
  if (h.count[0] == n) return 0;
  int left = 1;
  for (int l = 1; l <= 15; ++l) { left <<= 1; left -= h.count[l]; if (left < 0) return left; }
  uint16_t offs[16];
  offs[1] = 0;
  for (int l = 1; l < 15; ++l) offs[l + 1] = (uint16_t)(offs[l] + h.count[l]);
  for (int s = 0; s < n; ++s) if (len[s]) h.symbol[offs[len[s]]++] = (uint16_t)s;
  return left;
}

int huff_decode(BitIn& in, const Huff& h) {
  int code = 0, first = 0, index = 0;
  for (int l = 1; l <= 15; ++l) {
    code |= (int)in.bits(1);
    int count = h.count[l];
    if (code - count < first) return h.symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
    if (in.err) return -1;
  }
  return -1;
}

const uint16_t kLBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint16_t kLExt[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint16_t kDExt[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflate_codes(BitIn& in, OutWin& out, const Huff& lc, const Huff& dc) {
  for (;;) {
    int sym = huff_decode(in, lc);
    if (sym < 0 || in.err) return false;
    if (sym < 256) { out.put((uint8_t)sym); if (out.err) return false; continue; }
    if (sym == 256) return true;
    sym -= 257;
    if (sym >= 29) return false;
    int len = kLBase[sym] + (int)in.bits(kLExt[sym]);
    int ds = huff_decode(in, dc);
    if (ds < 0 || ds >= 30) return false;
    uint32_t dist = kDBase[ds] + in.bits(kDExt[ds]);
    if (dist > out.total || dist > 32768) return false;
    for (int i = 0; i < len; ++i) out.put(out.ring[(out.total - dist) & 0xFFFF]);
    if (out.err || in.err) return false;
  }
}

bool inflate_stream(BitIn& in, OutWin& out) {
  int last;
  do {
    last = (int)in.bits(1);
    int type = (int)in.bits(2);
    if (type == 0) {
      in.bitbuf = 0; in.bitcnt = 0;
      uint32_t len = (uint32_t)in.byte(); len |= (uint32_t)in.byte() << 8;
      uint32_t nlen = (uint32_t)in.byte(); nlen |= (uint32_t)in.byte() << 8;
      if (in.err || (len ^ 0xFFFF) != nlen) return false;
      for (uint32_t i = 0; i < len; ++i) { out.put((uint8_t)in.byte()); if (in.err || out.err) return false; }
    } else if (type == 1) {
      static Huff lc, dc;
      static bool init = false;
      if (!init) {
        uint8_t l[288];
        int s = 0;
        for (; s < 144; ++s) l[s] = 8;
        for (; s < 256; ++s) l[s] = 9;
        for (; s < 280; ++s) l[s] = 7;
        for (; s < 288; ++s) l[s] = 8;
        huff_build(lc, l, 288);
        uint8_t d[30];
        for (s = 0; s < 30; ++s) d[s] = 5;
        huff_build(dc, d, 30);
        init = true;
      }
      if (!inflate_codes(in, out, lc, dc)) return false;
    } else if (type == 2) {
      static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      int nlen = (int)in.bits(5) + 257, ndist = (int)in.bits(5) + 1, ncode = (int)in.bits(4) + 4;
      if (nlen > 286 || ndist > 30) return false;
      uint8_t lengths[320] = {};
      for (int i = 0; i < ncode; ++i) lengths[order[i]] = (uint8_t)in.bits(3);
      Huff cl;
      if (huff_build(cl, lengths, 19) != 0) return false;
      int idx = 0;
      uint8_t ll[320] = {};
      while (idx < nlen + ndist) {
        int sym = huff_decode(in, cl);
        if (sym < 0 || in.err) return false;
        if (sym < 16) ll[idx++] = (uint8_t)sym;
        else {
          int rep, val = 0;
          if (sym == 16) { if (idx == 0) return false; val = ll[idx - 1]; rep = 3 + (int)in.bits(2); }
          else if (sym == 17) rep = 3 + (int)in.bits(3);
          else rep = 11 + (int)in.bits(7);
          if (idx + rep > nlen + ndist) return false;
          while (rep--) ll[idx++] = (uint8_t)val;
        }
      }
      if (ll[256] == 0) return false;
      Huff lc, dc;
      int e = huff_build(lc, ll, nlen);
      if (e < 0 || (e > 0 && nlen - lc.count[0] != 1)) return false;
      e = huff_build(dc, ll + nlen, ndist);
      if (e < 0 || (e > 0 && ndist - dc.count[0] != 1)) return false;
      if (!inflate_codes(in, out, lc, dc)) return false;
    } else {
      return false;
    }
  } while (!last);
  out.drain();
  return !in.err && !out.err;
}

// ---------------------------------------------------------------------------------------------------------------------------
// A source: a folder or a zip, as a flat list of files.
struct SrcEntry {
  std::string rel;        // '/' separated, as stored
  uint64_t size = 0;
  int64_t mtime = 0;
  // zip only
  uint64_t lho = 0, csize = 0;
  uint16_t method = 0;
  uint32_t crc = 0;
};
struct Source {
  bool zip = false;
  fs::path path;
  std::vector<SrcEntry> entries;
  bool empty_input = false;
};

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

int64_t to_unix(fs::file_time_type t) {
  using namespace std::chrono;
  return (int64_t)duration_cast<seconds>(clock_cast<system_clock>(t).time_since_epoch()).count();
}

bool list_zip(const fs::path& zp, Source& src, std::string* err) {
  std::ifstream f(zp, std::ios::binary);
  if (!f) { *err = "The zip file could not be opened: " + S(zp); return false; }
  f.seekg(0, std::ios::end);
  const uint64_t fsize = (uint64_t)f.tellg();
  if (fsize < 22) { *err = "That file is not a zip archive (too small)."; return false; }
  const uint64_t tail = std::min<uint64_t>(fsize, 65557);
  std::vector<uint8_t> t((size_t)tail);
  f.seekg((std::streamoff)(fsize - tail));
  f.read((char*)t.data(), (std::streamsize)tail);
  int64_t e = -1;
  for (int64_t i = (int64_t)tail - 22; i >= 0; --i) if (rd32(&t[(size_t)i]) == 0x06054b50u) { e = i; break; }
  if (e < 0) { *err = "That file is not a zip archive (no end-of-archive record)."; return false; }
  uint64_t cd_off = rd32(&t[(size_t)e + 16]), cd_size = rd32(&t[(size_t)e + 12]), count = rd16(&t[(size_t)e + 10]);
  if (cd_off == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu || count == 0xFFFF) {
    if (e < 20 || rd32(&t[(size_t)e - 20]) != 0x07064b50u) { *err = "That zip file is damaged (zip64 record missing)."; return false; }
    uint64_t o64 = rd64(&t[(size_t)e - 20 + 8]);
    uint8_t z[56];
    f.seekg((std::streamoff)o64);
    f.read((char*)z, 56);
    if (f.gcount() != 56 || rd32(z) != 0x06064b50u) { *err = "That zip file is damaged (zip64 record unreadable)."; return false; }
    count = rd64(z + 32); cd_size = rd64(z + 40); cd_off = rd64(z + 48);
  }
  if (cd_off + cd_size > fsize || cd_size > (1ull << 30)) { *err = "That zip file is damaged (directory out of range)."; return false; }
  std::vector<uint8_t> cd((size_t)cd_size);
  f.seekg((std::streamoff)cd_off);
  f.read((char*)cd.data(), (std::streamsize)cd_size);
  if ((uint64_t)f.gcount() != cd_size) { *err = "That zip file is damaged (directory unreadable)."; return false; }
  size_t p = 0;
  for (uint64_t k = 0; k < count; ++k) {
    if (p + 46 > cd.size() || rd32(&cd[p]) != 0x02014b50u) { *err = "That zip file is damaged (bad directory entry)."; return false; }
    uint16_t flags = rd16(&cd[p + 8]), method = rd16(&cd[p + 10]);
    uint32_t crc = rd32(&cd[p + 16]);
    uint64_t cs = rd32(&cd[p + 20]), us = rd32(&cd[p + 24]), lho = rd32(&cd[p + 42]);
    size_t nl = rd16(&cd[p + 28]), el = rd16(&cd[p + 30]), cl = rd16(&cd[p + 32]);
    if (p + 46 + nl + el + cl > cd.size()) { *err = "That zip file is damaged (entry overruns the directory)."; return false; }
    std::string name((const char*)&cd[p + 46], nl);
    for (char& c : name) if (c == '\\') c = '/';
    const uint8_t* ex = &cd[p + 46 + nl];
    for (size_t q = 0; q + 4 <= el;) {
      uint16_t id = rd16(ex + q), sz = rd16(ex + q + 2);
      if (q + 4 + sz > el) break;
      if (id == 1) {
        size_t o = q + 4;
        if (us == 0xFFFFFFFFu && o + 8 <= q + 4 + sz) { us = rd64(ex + o); o += 8; }
        if (cs == 0xFFFFFFFFu && o + 8 <= q + 4 + sz) { cs = rd64(ex + o); o += 8; }
        if (lho == 0xFFFFFFFFu && o + 8 <= q + 4 + sz) { lho = rd64(ex + o); o += 8; }
      }
      q += 4 + sz;
    }
    p += 46 + nl + el + cl;
    if (name.empty() || name.back() == '/') continue;
    if (flags & 1) { *err = "That zip file is password protected."; return false; }
    SrcEntry se;
    se.rel = name; se.size = us; se.csize = cs; se.lho = lho; se.method = method; se.crc = crc;
    src.entries.push_back(se);
  }
  src.zip = true;
  src.path = zp;
  return true;
}

bool list_source(const std::string& input, Source& src, std::string* err) {
  fs::path ip = P(input);
  std::error_code ec;
  if (fs::is_directory(ip, ec)) {
    src.path = ip;
    size_t n = 0;
    for (fs::recursive_directory_iterator it(ip, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
      if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
      std::error_code e1;
      if (!it->is_regular_file(e1) || e1) continue;
      SrcEntry se;
      { auto u = it->path().lexically_relative(ip).generic_u8string(); se.rel.assign(u.begin(), u.end()); }
      se.size = (uint64_t)it->file_size(e1);
      se.mtime = to_unix(it->last_write_time(e1));
      src.entries.push_back(se);
      if (++n > 50000) break;
    }
    return true;
  }
  if (fs::is_regular_file(ip, ec)) {
    std::string ext = lower(S(ip.extension()));
    if (ext != ".zip") { *err = "Pick a folder of save files or a .zip of one. \"" + S(ip.filename()) + "\" is neither."; return false; }
    return list_zip(ip, src, err);
  }
  *err = "That path does not exist: " + input;
  return false;
}

// Copies entry `e` of `src` to `dst` (overwriting), checking size (and the CRC for zip entries).
bool extract_entry(const Source& src, const SrcEntry& e, const fs::path& dst, std::string* err) {
  std::error_code ec;
  if (!src.zip) {
    fs::copy_file(src.path / P(e.rel), dst, fs::copy_options::overwrite_existing, ec);
    if (ec) { *err = "Could not copy " + e.rel + ": " + ec.message(); return false; }
    if (fs::file_size(dst, ec) != e.size || ec) { *err = "The copy of " + e.rel + " has the wrong size (disk full?)."; return false; }
    return true;
  }
  std::ifstream in(src.path, std::ios::binary);
  uint8_t lh[30];
  in.seekg((std::streamoff)e.lho);
  in.read((char*)lh, 30);
  if (in.gcount() != 30 || rd32(lh) != 0x04034b50u) { *err = "The zip file is damaged (entry " + e.rel + " not found)."; return false; }
  in.seekg((std::streamoff)(e.lho + 30 + rd16(lh + 26) + rd16(lh + 28)));
  std::ofstream out(dst, std::ios::binary | std::ios::trunc);
  if (!out) { *err = "Could not write " + S(dst); return false; }
  uint32_t crc = 0;
  uint64_t written = 0;
  bool ok = false;
  if (e.method == 0) {
    std::vector<uint8_t> b(1 << 20);
    uint64_t left = e.csize;
    ok = true;
    while (left && ok) {
      size_t n = (size_t)std::min<uint64_t>(left, b.size());
      in.read((char*)b.data(), (std::streamsize)n);
      if ((size_t)in.gcount() != n) { ok = false; break; }
      out.write((const char*)b.data(), (std::streamsize)n);
      crc = crc32_update(crc, b.data(), n);
      left -= n; written += n;
    }
  } else if (e.method == 8) {
    BitIn bi(in, e.csize);
    OutWin ow(out);
    ok = inflate_stream(bi, ow);
    crc = ow.crc; written = ow.total;
  } else {
    *err = "The zip file uses an unsupported compression method for " + e.rel + ".";
    return false;
  }
  out.flush();
  if (!ok || !out) { *err = "The zip file is damaged or the disk is full (while reading " + e.rel + ")."; return false; }
  if (written != e.size || crc != e.crc) { *err = "The zip file is damaged: " + e.rel + " does not match its checksum."; return false; }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Locating and validating the game's save files inside a source.
struct Found {
  std::string dir;                                   // directory inside the source ("" = top)
  std::map<std::string, const SrcEntry*> files;      // expected name -> entry
  std::vector<std::string> extra;                    // other files beside them
  bool old_version = false;
};

std::string parent_of(const std::string& rel) {
  size_t k = rel.rfind('/');
  return k == std::string::npos ? std::string() : rel.substr(0, k);
}
std::string leaf_of(const std::string& rel) {
  size_t k = rel.rfind('/');
  return k == std::string::npos ? rel : rel.substr(k + 1);
}

bool locate(const Source& src, Found* out, std::vector<std::string>* warnings, std::string* err) {
  if (src.entries.empty()) {
    *err = src.zip ? "That zip file has no files in it." : "That folder has no files in it.";
    return false;
  }
  std::map<std::string, size_t> idx;   // expected name -> position
  for (size_t i = 0; i < expected_files().size(); ++i) idx[expected_files()[i].name] = i;
  struct Group { std::string dir; std::map<std::string, const SrcEntry*> files; std::vector<std::string> extra; int64_t newest = 0; };
  std::map<std::string, Group> groups;
  size_t considered = 0;
  for (const SrcEntry& e : src.entries) {
    if (e.rel.rfind("__MACOSX/", 0) == 0 || e.rel.find("/__MACOSX/") != std::string::npos) continue;
    ++considered;
    Group& g = groups[parent_of(e.rel)];
    g.dir = parent_of(e.rel);
    std::string nm = lower(leaf_of(e.rel));
    if (idx.count(nm)) { g.files[nm] = &e; g.newest = std::max(g.newest, e.mtime); }
    else g.extra.push_back(leaf_of(e.rel));
  }
  Group* best = nullptr;
  int nfull = 0;
  auto rank = [](const Group& g, bool exact) {
    // more matches, then a folder named "0" (Ryujinx's committed copy; "1" is the working copy), then newest, then name
    return std::make_tuple((int)g.files.size(), exact ? 1 : 0, g.newest);
  };
  for (auto& kv : groups) {
    Group& g = kv.second;
    if (g.files.empty()) continue;
    if (g.files.size() == expected_files().size()) ++nfull;
    if (!best) { best = &g; continue; }
    const bool gz = leaf_of(g.dir) == "0", bz = leaf_of(best->dir) == "0";
    auto a = rank(g, gz), b = rank(*best, bz);
    if (a > b || (a == b && g.dir > best->dir)) best = &g;
  }
  if (!best) {
    *err = std::string(src.zip ? "That zip file" : "That folder") + " does not hold New Pokemon Snap save files. This game's save is 15 files with names like " +
           "00c31e53dde3ac51 (16 hex digits); none of the " + std::to_string(considered) + " files found has such a name. " +
           "A save for another game, or a copy of this game's data (not its save), cannot be imported.";
    return false;
  }
  if (nfull > 1 && warnings) warnings->push_back(std::to_string(nfull) + " complete saves were found; used \"" + (best->dir.empty() ? "(top level)" : best->dir) + "\".");
  if (best->files.size() < expected_files().size()) {
    std::string miss;
    int n = 0;
    for (const FileSpec& f : expected_files()) if (!best->files.count(f.name)) { if (n++ < 4) miss += (miss.empty() ? "" : ", ") + std::string(f.name); }
    if (n > 4) miss += ", and " + std::to_string(n - 4) + " more";
    *err = "This looks like a New Pokemon Snap save, but it is incomplete: " + std::to_string(best->files.size()) + " of " +
           std::to_string(expected_files().size()) + " files are there (missing " + miss + "). Back up the whole save folder and try again.";
    return false;
  }
  // sizes
  int cur = 0, old = 0;
  std::string bad;
  for (const FileSpec& f : expected_files()) {
    const uint64_t sz = best->files[f.name]->size;
    const bool c = sz == f.size, o = sz == f.old_size;
    if (!c && !o) {
      if (bad.empty()) bad = std::string(f.name) + " is " + std::to_string(sz) + " bytes (expected " + std::to_string(f.size) +
                             (f.old_size != f.size ? " or " + std::to_string(f.old_size) : std::string()) + ")";
    }
    if (c && !o) ++cur;
    if (o && !c) ++old;
  }
  if (!bad.empty()) {
    *err = "The file names match New Pokemon Snap, but the sizes do not, so this is not a save this game can load: " + bad + ". It may be damaged or from a different game version.";
    return false;
  }
  if (cur && old) {
    *err = "The files mix sizes from two different save layouts, so this is not one consistent save. Re-copy the whole save from one backup.";
    return false;
  }
  out->dir = best->dir;
  out->files = best->files;
  out->extra = best->extra;
  out->old_version = old > 0;
  if (out->old_version && warnings)
    warnings->push_back("This save comes from an older game version (smaller album files). The game upgrades it itself the first time it loads; keep the backup if you want to go back.");
  if (!best->extra.empty() && warnings) warnings->push_back(std::to_string(best->extra.size()) + " other file(s) beside the save were left out (" + best->extra.front() + (best->extra.size() > 1 ? ", ..." : "") + ").");
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
void flush_path(const fs::path& p) {
#ifdef _WIN32
  HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) { FlushFileBuffers(h); CloseHandle(h); }
#else
  (void)p;
#endif
}
bool replace_file(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
  return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code ec;
  fs::rename(from, to, ec);
  return !ec;
#endif
}
bool gone(const fs::path& p) {
  std::error_code ec;
  return fs::symlink_status(p, ec).type() == fs::file_type::not_found;
}
bool remove_checked(const fs::path& p) {
  std::error_code ec;
  fs::remove_all(p, ec);
  return !ec && gone(p);
}

// Same order as the runtime's drop_generation: the marker goes first, then the directory.
void drop_generation(const fs::path& uid, uint64_t g) {
  if (!gone(sg::gen_ok(uid, g))) {
    std::ofstream(sg::gen_orphan(uid, g)) << "orphan\n";
    remove_checked(sg::gen_ok(uid, g));
  }
  if (remove_checked(sg::gen_dir(uid, g))) { std::error_code ec; fs::remove(sg::gen_orphan(uid, g), ec); }
}

std::set<uint64_t> gen_numbers(const fs::path& uid) {
  std::set<uint64_t> v;
  std::error_code ec;
  for (fs::directory_iterator it(uid / "gens", ec), end; !ec && it != end; it.increment(ec)) {
    const std::string stem = S(it->path().stem());
    if (stem.empty() || stem.find_first_not_of("0123456789") != std::string::npos) continue;
    v.insert(std::strtoull(stem.c_str(), nullptr, 10));
  }
  return v;
}

// A uid folder under save_root that holds a loadable save (first one by name), else empty.
std::string find_uid_dir(const fs::path& root) {
  std::error_code ec;
  std::vector<std::string> names;
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e1;
    uint64_t n = 0;
    if (it->is_directory(e1) && !e1 && sg::committed_gen(it->path(), &n)) names.push_back(S(it->path().filename()));
  }
  std::sort(names.begin(), names.end());
  return names.empty() ? std::string() : names.front();
}

#ifdef _WIN32
// The running game holds an exclusive byte lock on <save_root>/.snap.lock (fs_core.cpp mount_save). Take the same lock for the
// import so the game cannot start mid-write; false = held by a game.
struct FolderLock {
  HANDLE h = INVALID_HANDLE_VALUE;
  bool taken = false;
  bool open(const fs::path& root) {
    std::error_code ec;
    fs::create_directories(root, ec);
    h = CreateFileW((root / ".snap.lock").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    OVERLAPPED ov{};
    taken = LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov) != 0;
    if (!taken) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
    return taken;
  }
  ~FolderLock() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
#else
struct FolderLock { bool open(const fs::path&) { return true; } };
#endif

const char* kDefaultUid = "534e4150524543310000000000000001";

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------
bool import_save(const std::string& input, const std::string& save_root, const std::string& uid_in, Report* rep, std::string* err,
                 bool* refused_running, int keep) {
  Report local;
  Report& R = rep ? *rep : local;
  std::string e2;
  std::string& E = err ? *err : e2;
  if (refused_running) *refused_running = false;
  Source src;
  if (!list_source(input, src, &E)) return false;
  Found f;
  if (!locate(src, &f, &R.warnings, &E)) return false;
  R.source = f.dir.empty() ? std::string("top level of the ") + (src.zip ? "zip" : "folder") : f.dir;
  R.old_version = f.old_version;

  const fs::path root = P(save_root);
  FolderLock lock;
  std::error_code ec;
  if (!lock.open(root)) {
    if (refused_running) *refused_running = true;
    E = "The game is running (it holds this save folder). Close it, then import again. Nothing was changed.";
    return false;
  }
  std::string uid_name = uid_in;
  if (uid_name.empty()) uid_name = find_uid_dir(root);
  if (uid_name.empty()) uid_name = kDefaultUid;
  const fs::path uid = root / P(uid_name);
  fs::create_directories(uid / "gens", ec);
  if (ec) { E = "Could not create the save folder " + S(uid) + ": " + ec.message(); return false; }

  uint64_t cur = 0, n = 0;
  bool pointed = false;
  R.had_previous = sg::committed_gen(uid, &cur, &pointed);
  R.previous = cur;
  std::set<uint64_t> have = gen_numbers(uid);
  n = have.empty() ? 1 : std::max<uint64_t>(*have.rbegin(), R.had_previous ? cur : 0) + 1;
  // never build on a leftover with this number
  remove_checked(sg::gen_ok(uid, n));
  remove_checked(sg::gen_orphan(uid, n));
  if (!remove_checked(sg::gen_dir(uid, n))) { E = "A leftover folder in the save store could not be removed: " + S(sg::gen_dir(uid, n)); return false; }
  const fs::path ng = sg::gen_dir(uid, n);
  fs::create_directories(ng, ec);
  if (ec) { E = "Could not create " + S(ng) + ": " + ec.message(); return false; }
  auto fail = [&](const std::string& why) {
    E = why + " The current save was not changed.";
    drop_generation(uid, n);
    return false;
  };
  uint64_t bytes = 0;
  for (const FileSpec& fs_ : expected_files()) {
    const SrcEntry& e = *f.files[fs_.name];
    const fs::path dst = ng / fs_.name;
    std::string why;
    if (!extract_entry(src, e, dst, &why)) return fail(why);
    flush_path(dst);
    bytes += e.size;
  }
  {
    std::ofstream ok(sg::gen_ok(uid, n), std::ios::binary);
    ok << "ok\n";
    ok.flush();
    if (!ok) return fail("Could not write the generation marker.");
  }
  flush_path(sg::gen_ok(uid, n));
  {
    const fs::path tmp = uid / "CURRENT.tmp";
    {
      std::ofstream pf(tmp, std::ios::binary | std::ios::trunc);
      pf << sg::pointer_text(n);
      pf.flush();
      if (!pf) return fail("Could not write the save pointer.");
    }
    flush_path(tmp);
    if (!replace_file(tmp, uid / "CURRENT")) return fail("Could not publish the new save (" + S(uid / "CURRENT") + " is locked).");
  }
  // from here the import is committed; backups: keep the new generation and the `keep` newest valid ones before it
  {
    std::set<uint64_t> nums, valid;
    for (uint64_t g : gen_numbers(uid)) if (g < n) { nums.insert(g); if (sg::gen_valid(uid, g)) valid.insert(g); }
    int kept = 0;
    for (auto it = valid.rbegin(); it != valid.rend() && kept < keep; ++it, ++kept) nums.erase(*it);
    for (uint64_t g : nums) drop_generation(uid, g);
  }
  R.generation = n;
  R.bytes = bytes;
  R.files = (int)expected_files().size();
  R.dest = S(uid);
  return true;
}

bool export_save(const std::string& save_root, const std::string& uid_in, const std::string& dest, Report* rep, std::string* err) {
  Report local;
  Report& R = rep ? *rep : local;
  std::string e2;
  std::string& E = err ? *err : e2;
  const fs::path root = P(save_root);
  std::string uid_name = uid_in.empty() ? find_uid_dir(root) : uid_in;
  uint64_t cur = 0;
  if (uid_name.empty() || !sg::committed_gen(root / P(uid_name), &cur)) { E = "There is no save to export yet (nothing in " + save_root + ")."; return false; }
  const fs::path gd = sg::gen_dir(root / P(uid_name), cur);
  // the files first, so a damaged store is reported before anything is written
  std::vector<std::pair<std::string, uint64_t>> files;
  for (const FileSpec& fsp : expected_files()) {
    std::error_code ec;
    const uint64_t sz = fs::file_size(gd / fsp.name, ec);
    if (ec) { E = std::string("The current save is missing ") + fsp.name + "; nothing was exported."; return false; }
    files.push_back({fsp.name, sz});
  }
  fs::path out = P(dest);
  std::error_code ec;
  bool direct = !fs::exists(out, ec);
  if (!direct && fs::is_directory(out, ec)) direct = fs::directory_iterator(out, ec) == fs::directory_iterator();
  else if (!direct) { E = "That path is a file, not a folder: " + dest; return false; }
  if (!direct) {
    std::time_t t = std::time(nullptr);
    std::tm m{};
#ifdef _WIN32
    localtime_s(&m, &t);
#else
    localtime_r(&t, &m);
#endif
    char b[64];
    std::snprintf(b, sizeof b, "New Pokemon Snap save %04d-%02d-%02d %02d%02d%02d", m.tm_year + 1900, m.tm_mon + 1, m.tm_mday, m.tm_hour, m.tm_min, m.tm_sec);
    out /= b;
  }
  fs::create_directories(out, ec);
  if (ec) { E = "Could not create " + S(out) + ": " + ec.message(); return false; }
  uint64_t bytes = 0;
  for (auto& [name, sz] : files) {
    fs::copy_file(gd / name, out / name, fs::copy_options::overwrite_existing, ec);
    if (ec || fs::file_size(out / name, ec) != sz) { E = "Could not copy " + name + " to " + S(out) + " (disk full?)."; return false; }
    flush_path(out / name);
    bytes += sz;
  }
  R.dest = S(out);
  R.generation = cur;
  R.bytes = bytes;
  R.files = (int)files.size();
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
std::vector<Candidate> detect_emulator_saves(const std::string& appdata_in) {
  std::vector<Candidate> out;
  std::string ad = appdata_in;
#ifdef _WIN32
  if (ad.empty()) ad = appdata_utf8();
#endif
  if (ad.empty()) return out;
  auto consider = [&](const fs::path& dir, const std::string& label) {
    Source src;
    std::string err;
    if (!list_source(S(dir), src, &err)) return;
    // only this folder's own files (no recursion into sub-saves)
    Source flat;
    flat.path = src.path;
    for (auto& e : src.entries) if (e.rel.find('/') == std::string::npos) flat.entries.push_back(e);
    Found f;
    if (!locate(flat, &f, nullptr, &err)) return;
    Candidate c;
    c.label = label;
    c.path = S(dir);
    c.old_version = f.old_version;
    for (auto& e : flat.entries) c.mtime = std::max(c.mtime, e.mtime);
    out.push_back(c);
  };
  std::error_code ec;
  const fs::path adp = P(ad);
  // Ryujinx: save ids are not tied to the title in any file we can read reliably, so a save folder counts by its contents
  {
    const fs::path base = adp / "Ryujinx" / "bis" / "user" / "save";
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e1;
      if (!it->is_directory(e1)) continue;
      const std::string id = S(it->path().filename());
      if (id.size() != 16 || id.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) continue;
      consider(it->path() / "0", "Ryujinx, save " + id);
    }
  }
  static const char* kEmus[] = {"yuzu", "suyu", "sudachi", "citron", "eden"};
  for (const char* emu : kEmus) {
    const fs::path base = adp / emu / "nand" / "user" / "save" / "0000000000000000";
    ec.clear();
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e1;
      if (!it->is_directory(e1)) continue;
      const fs::path d = it->path() / kTitleId;
      if (fs::is_directory(d, e1)) consider(d, std::string(emu) + ", user " + S(it->path().filename()));
    }
  }
  std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) { return a.mtime > b.mtime; });
  return out;
}

// ---------------------------------------------------------------------------------------------------------------------------
std::string game_uid_dir() {
  const Config& c = config();
  return uid_dir_name(c.uid);
}

std::string target_save_root(const std::string& profile, std::string* label) { return tool_save_root(profile, label); }

static std::string fmt_size(uint64_t b) {
  char t[40];
  if (b >= (1ull << 30)) std::snprintf(t, sizeof t, "%.2f GB", (double)b / (1 << 30));
  else std::snprintf(t, sizeof t, "%.1f MB", (double)b / (1 << 20));
  return t;
}

std::string format_report(const Report& r, bool import) {
  std::string s;
  if (import) {
    s = "Imported " + std::to_string(r.files) + " save files (" + fmt_size(r.bytes) + ") from " + r.source + ".\nThey are the current save now (generation " + std::to_string(r.generation) + ").";
    if (r.had_previous) s += "\nYour previous save was kept as a backup (generation " + std::to_string(r.previous) + ").";
  } else {
    s = "Exported " + std::to_string(r.files) + " save files (" + fmt_size(r.bytes) + ") to:\n" + r.dest +
        "\nRestore that folder with a save manager such as JKSV or Checkpoint, or copy its files into the game's save folder of an emulator.";
  }
  for (const std::string& w : r.warnings) s += "\nNote: " + w;
  return s;
}

int run_cli(const std::string& cmd, const std::string& path, const std::string& profile) {
  std::string label, err;
  const std::string root = target_save_root(profile, &label);
  if (root.empty()) { std::fprintf(stderr, "save: no save folder could be chosen (%s)\n", label.c_str()); return 1; }
  Report r;
  if (cmd == "import") {
    bool running = false;
    std::fprintf(stdout, "Importing into %s (%s)\n", label.c_str(), root.c_str());
    if (!import_save(path, root, game_uid_dir(), &r, &err, &running)) { std::fprintf(stderr, "Import failed: %s\n", err.c_str()); return running ? 2 : 1; }
    std::fprintf(stdout, "%s\n", format_report(r, true).c_str());
    return 0;
  }
  if (!export_save(root, std::string(), path, &r, &err)) { std::fprintf(stderr, "Export failed: %s\n", err.c_str()); return 1; }
  std::fprintf(stdout, "%s\n", format_report(r, false).c_str());
  return 0;
}

}  // namespace saveport
}  // namespace hle
