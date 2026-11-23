// SPDX-License-Identifier: MIT
// Profile backups to a sync folder (see sync_backup.h and docs/sync-backups.md).
#include "sync_backup.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "hle.h"
#include "hle_config.h"
#include "json_lite.h"
#include "profiles.h"
#include "save_backup.h"
#include "save_gen.h"
#include "zip_read.h"

namespace hle {
namespace syncbackup {

namespace fs = std::filesystem;
namespace sg = hle::savegen;

static fs::path P(const std::string& s) { return fs::u8path(s); }
static std::string S(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }
static std::string G(const fs::path& p) { auto u = p.generic_u8string(); return std::string(u.begin(), u.end()); }

static std::string env_u8(const char* name) {
#ifdef _WIN32
  std::wstring w;
  for (const char* c = name; *c; ++c) w.push_back((wchar_t)*c);
  DWORD n = GetEnvironmentVariableW(w.c_str(), nullptr, 0);
  if (!n) return "";
  std::wstring v(n, L'\0');
  DWORD got = GetEnvironmentVariableW(w.c_str(), v.data(), n);
  v.resize(got);
  return S(fs::path(v));
#else
  const char* e = std::getenv(name);
  return e ? e : "";
#endif
}

// ---- SHA-256 (FIPS 180-4) ------------------------------------------------------------------------------------------------------------

namespace {

struct Sha {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t buf[64];
  size_t blen = 0;
  uint64_t total = 0;
  static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void block(const uint8_t* p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void update(const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    total += n;
    while (n) {
      if (blen == 0 && n >= 64) { block(p); p += 64; n -= 64; continue; }
      size_t t = std::min(n, 64 - blen);
      std::memcpy(buf + blen, p, t);
      blen += t; p += t; n -= t;
      if (blen == 64) { block(buf); blen = 0; }
    }
  }
  std::string hex() {
    const uint64_t bits = total * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t z = 0;
    while (blen != 56) update(&z, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(len, 8);
    char out[65];
    for (int i = 0; i < 8; ++i) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
    return std::string(out, 64);
  }
};

uint64_t g_zip64 = 0xFFFFFFFFull;
bool g_corrupt_part = false;

}  // namespace

std::string sha256_hex(const void* data, size_t n) {
  Sha s;
  s.update(data, n);
  return s.hex();
}
void set_zip64_threshold_for_test(uint64_t t) { g_zip64 = t; }
void set_corrupt_part_for_test(bool on) { g_corrupt_part = on; }

// ---- settings ------------------------------------------------------------------------------------------------------------------------

std::string config_dir() {
  std::string d = env_u8("SNAP_CONFIG_DIR");
  if (!d.empty()) return d;
  d = env_u8("APPDATA");
  if (!d.empty()) return S(P(d) / "SnapRecomp");
  return config().data_root;
}

std::string settings_path(const std::string& dir) { return S(P(dir) / "backup.json"); }

static bool read_text(const fs::path& p, std::string* out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

static bool replace_file(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
  return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code ec;
  fs::rename(from, to, ec);
  return !ec;
#endif
}

static bool write_text_atomic(const fs::path& p, const std::string& text) {
  std::error_code ec;
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  const fs::path tmp = p.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f.write(text.data(), (std::streamsize)text.size());
    f.flush();
    if (!f) { fs::remove(tmp, ec); return false; }
  }
  if (!replace_file(tmp, p)) { fs::remove(tmp, ec); return false; }
  return true;
}

static int clamp_keep(long v) { return (int)std::max(1l, std::min((long)kMaxKeep, v)); }

static bool truthy(const std::string& s) { return s == "1" || s == "on" || s == "true" || s == "yes"; }

static void parse_include(const std::string& list, Settings* s) {
  s->saves = s->settings = s->photos = false;
  size_t i = 0;
  while (i <= list.size()) {
    size_t j = list.find_first_of(", ;", i);
    if (j == std::string::npos) j = list.size();
    std::string w = list.substr(i, j - i);
    for (char& c : w) c = (char)std::tolower((unsigned char)c);
    if (w == "saves" || w == "save") s->saves = true;
    else if (w == "settings") s->settings = true;
    else if (w == "photos") s->photos = true;
    i = j + 1;
  }
}

Settings read_settings(const std::string& dir, bool with_env) {
  Settings s;
  std::string text;
  if (read_text(P(settings_path(dir)), &text)) {
    Json j;
    std::string err;
    JsonParser jp(text);
    if (jp.parse(j, err) && j.type == Json::Obj) {
      if (const Json* x = j.get("enabled")) s.enabled = x->boolean(false);
      if (const Json* x = j.get("folder")) s.folder = x->str();
      if (const Json* x = j.get("keep")) s.keep = clamp_keep((long)x->num(kDefaultKeep));
      if (const Json* x = j.get("saves")) s.saves = x->boolean(true);
      if (const Json* x = j.get("settings")) s.settings = x->boolean(true);
      if (const Json* x = j.get("photos")) s.photos = x->boolean(true);
    }
  }
  if (with_env) {
    std::string v;
    if (!(v = env_u8("SNAP_SYNC_BACKUP")).empty()) s.enabled = truthy(v);
    if (!(v = env_u8("SNAP_SYNC_BACKUP_FOLDER")).empty()) s.folder = v;
    if (!(v = env_u8("SNAP_SYNC_BACKUP_KEEP")).empty()) s.keep = clamp_keep(std::atol(v.c_str()));
    if (!(v = env_u8("SNAP_SYNC_BACKUP_INCLUDE")).empty()) parse_include(v, &s);
  }
  return s;
}

bool write_settings(const std::string& dir, const Settings& s, std::string* err) {
  Json j = Json::object();
  j.set("enabled", Json::boolean_(s.enabled));
  j.set("folder", Json::string(s.folder));
  j.set("keep", Json::number(clamp_keep(s.keep)));
  j.set("saves", Json::boolean_(s.saves));
  j.set("settings", Json::boolean_(s.settings));
  j.set("photos", Json::boolean_(s.photos));
  std::string text;
  json_dump(j, text);
  text += "\n";
  if (!write_text_atomic(P(settings_path(dir)), text)) { if (err) *err = "Could not write " + settings_path(dir); return false; }
  return true;
}

std::string include_text(const Settings& s) {
  std::string r;
  auto add = [&](bool on, const char* n) { if (on) { if (!r.empty()) r += ", "; r += n; } };
  add(s.saves, "saves"); add(s.settings, "settings"); add(s.photos, "photos");
  return r.empty() ? "nothing" : r;
}

// ---- source ----------------------------------------------------------------------------------------------------------------------------

static std::string cfg_value(const fs::path& cfg, const std::string& key) {
  std::string text;
  if (!read_text(cfg, &text)) return "";
  size_t i = 0;
  while (i < text.size()) {
    size_t e = text.find('\n', i);
    if (e == std::string::npos) e = text.size();
    std::string line = text.substr(i, e - i);
    i = e + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line.substr(0, eq) != key) continue;
    return line.substr(eq + 1);
  }
  return "";
}

bool resolve_source(const std::string& profile, Source* out, std::string* err) {
  std::string label;
  const std::string root = tool_save_root(profile, &label);
  if (root.empty()) { if (err) *err = "No save folder could be chosen: " + label; return false; }
  Source s;
  s.save_root = root;
  const Config& c = config();
  fs::path sr = P(root);
  while (!sr.empty() && (sr.filename().empty() || sr.filename() == ".")) sr = sr.parent_path();
  const fs::path pdir = sr.parent_path();
  std::error_code ec;
  if (sr.filename() == "save" && fs::exists(pdir.parent_path() / "profiles.json", ec)) {
    s.profiles_root = S(pdir.parent_path());
    s.profile_id = S(pdir.filename());
    s.profile_name = s.profile_id;
    profiles::Store st(s.profiles_root);
    st.load();
    if (const profiles::Profile* p = st.find(s.profile_id)) s.profile_name = p->name;
    s.profile_cfg = S(pdir / "settings.cfg");
  } else {
    s.profile_cfg = S(P(c.data_root) / "settings.cfg");
  }
  const fs::path cd = P(config_dir());
  for (const char* n : {"window.json", "audio.json", "input.json"}) s.globals.push_back({n, S(cd / n)});
  std::string photos = env_u8("SNAP_PHOTO_DIR");
  if (photos.empty()) photos = cfg_value(P(s.profile_cfg), "photo_dir");
  if (photos.empty()) photos = S(P(c.data_root) / "photos");
  s.photo_dirs.push_back({"photos", photos});
  s.photo_dirs.push_back({"screenshots", c.screenshot_dir.empty() ? S(P(c.data_root) / "screenshots") : c.screenshot_dir});
  *out = s;
  return true;
}

// ---- save health -----------------------------------------------------------------------------------------------------------------------

std::string SaveState::signature() const {
  std::string r;
  for (const auto& g : gens) { if (!r.empty()) r += ";"; r += g.first + ":" + std::to_string(g.second); }
  return r;
}

static bool is_number(const std::string& s) { return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos; }

SaveState inspect_save(const std::string& save_root) {
  SaveState st;
  std::error_code ec;
  const fs::path root = P(save_root);
  if (save_root.empty() || !fs::is_directory(root, ec)) { st.health = Health::NoSave; return st; }
  if (profiles::save_in_use(save_root)) {
    st.health = Health::InUse;
    st.detail = "The game is running (it holds the save folder).";
    return st;
  }
  std::string mid, recover;
  if (!savebackup::queued_restore(save_root).empty()) recover = "A restore is queued for the next start.";
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_directory(e2)) continue;
    const std::string uid = S(it->path().filename());
    if (uid.empty() || uid[0] == '.') continue;
    const fs::path ud = it->path();
    uint64_t n = 0;
    bool pointed = true;
    const bool has = sg::committed_gen(ud, &n, &pointed);
    bool any_gen = false;
    for (fs::directory_iterator g(ud / "gens", e2), ge; !e2 && g != ge; g.increment(e2)) {
      std::error_code e3;
      const std::string name = S(g->path().filename());
      if (g->is_directory(e3) && is_number(name)) {
        any_gen = true;
        if (!fs::exists(sg::gen_ok(ud, std::strtoull(name.c_str(), nullptr, 10)), e3) && mid.empty())
          mid = "Generation " + name + " has no completion marker: the game stopped in the middle of a save.";
      } else if (g->path().extension() == ".orphan" && recover.empty()) {
        recover = "A save generation is marked orphaned (" + name + "); start the game once so it can clean that up.";
      }
    }
    if (fs::exists(ud / "CURRENT.tmp", e2) && mid.empty()) mid = "The save pointer was being replaced (CURRENT.tmp is still there).";
    if (!has) {
      if (any_gen || fs::exists(ud / "CURRENT", e2)) { if (recover.empty()) recover = "The save has no complete generation; start the game once so it can recover."; }
      continue;
    }
    if (!pointed && recover.empty()) recover = "The save pointer (CURRENT) is missing or damaged; start the game once so it can repair it.";
    st.gens.push_back({uid, n});
  }
  std::sort(st.gens.begin(), st.gens.end());
  if (!mid.empty()) { st.health = Health::MidWrite; st.detail = mid; }
  else if (!recover.empty()) { st.health = Health::NeedsRecovery; st.detail = recover; }
  else if (st.gens.empty()) st.health = Health::NoSave;
  else st.health = Health::Ok;
  return st;
}

// ---- zip writer (streaming, ZIP64) ---------------------------------------------------------------------------------------------------

namespace {

void put16(std::string& s, uint32_t v) { s.push_back((char)(v & 255)); s.push_back((char)((v >> 8) & 255)); }
void put32(std::string& s, uint32_t v) { put16(s, v & 0xFFFF); put16(s, v >> 16); }
void put64(std::string& s, uint64_t v) { put32(s, (uint32_t)v); put32(s, (uint32_t)(v >> 32)); }

void dos_time(int64_t t, uint32_t* dt, uint32_t* dd) {
  std::time_t tt = (std::time_t)t;
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  int year = std::max(1980, tm.tm_year + 1900);
  *dt = (uint32_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
  *dd = (uint32_t)(((year - 1980) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}

struct ZEntry {
  std::string name;
  uint64_t size = 0, lho = 0;
  uint32_t crc = 0;
  bool z64 = false;   // sizes live in a zip64 extra field
  uint32_t dt = 0, dd = 0;
};

class ZipWriter {
 public:
  bool open(const fs::path& p) {
    buf_.assign(1 << 20, 0);
    f_.rdbuf()->pubsetbuf(buf_.data(), (std::streamsize)buf_.size());
    f_.open(p, std::ios::binary | std::ios::trunc);
    return (bool)f_;
  }
  uint64_t offset() const { return off_; }
  bool begin(const std::string& name, uint64_t size, int64_t mtime) {
    cur_ = ZEntry();
    cur_.name = name;
    cur_.size = size;
    cur_.lho = off_;
    cur_.z64 = size >= g_zip64;
    dos_time(mtime, &cur_.dt, &cur_.dd);
    crc_ = 0;
    written_ = 0;
    std::string h;
    put32(h, 0x04034b50u);
    put16(h, cur_.z64 ? 45 : 20);
    put16(h, 0x0808);   // data descriptor follows, names are UTF-8
    put16(h, 0);        // stored
    put16(h, cur_.dt); put16(h, cur_.dd);
    put32(h, 0);                                // crc: in the descriptor
    put32(h, cur_.z64 ? 0xFFFFFFFFu : 0);       // sizes: in the descriptor (zip64: the marker)
    put32(h, cur_.z64 ? 0xFFFFFFFFu : 0);
    put16(h, (uint32_t)name.size());
    put16(h, cur_.z64 ? 20 : 0);
    h += name;
    if (cur_.z64) { put16(h, 1); put16(h, 16); put64(h, 0); put64(h, 0); }
    return raw(h.data(), h.size());
  }
  bool write(const uint8_t* p, size_t n) {
    crc_ = hle::zipr::crc32_update(crc_, p, n);
    written_ += n;
    return raw(p, n);
  }
  uint64_t written() const { return written_; }
  bool end() {
    if (written_ != cur_.size) return false;
    cur_.crc = crc_;
    std::string d;
    put32(d, 0x08074b50u);
    put32(d, cur_.crc);
    if (cur_.z64) { put64(d, cur_.size); put64(d, cur_.size); }
    else { put32(d, (uint32_t)cur_.size); put32(d, (uint32_t)cur_.size); }
    if (!raw(d.data(), d.size())) return false;
    es_.push_back(cur_);
    return true;
  }
  bool add_bytes(const std::string& name, const std::string& data, int64_t mtime) {
    return begin(name, data.size(), mtime) && write((const uint8_t*)data.data(), data.size()) && end();
  }
  bool finish() {
    const uint64_t cd_off = off_;
    for (const ZEntry& e : es_) {
      const bool zo = e.lho >= g_zip64;
      std::string x;
      if (e.z64 || zo) {
        std::string body;
        if (e.z64) { put64(body, e.size); put64(body, e.size); }
        if (zo) put64(body, e.lho);
        put16(x, 1); put16(x, (uint32_t)body.size()); x += body;
      }
      std::string c;
      put32(c, 0x02014b50u);
      put16(c, 45);
      put16(c, (e.z64 || zo) ? 45 : 20);
      put16(c, 0x0808);
      put16(c, 0);
      put16(c, e.dt); put16(c, e.dd);
      put32(c, e.crc);
      put32(c, e.z64 ? 0xFFFFFFFFu : (uint32_t)e.size);
      put32(c, e.z64 ? 0xFFFFFFFFu : (uint32_t)e.size);
      put16(c, (uint32_t)e.name.size());
      put16(c, (uint32_t)x.size());
      put16(c, 0); put16(c, 0); put16(c, 0);   // comment, disk, internal attributes
      put32(c, 0);                              // external attributes
      put32(c, zo ? 0xFFFFFFFFu : (uint32_t)e.lho);
      c += e.name;
      c += x;
      if (!raw(c.data(), c.size())) return false;
    }
    const uint64_t cd_size = off_ - cd_off;
    const uint64_t count = es_.size();
    std::string t;
    const bool big = count >= 0xFFFF || cd_off >= g_zip64 || cd_size >= g_zip64;
    if (big) {
      const uint64_t z_off = off_;
      put32(t, 0x06064b50u); put64(t, 44); put16(t, 45); put16(t, 45); put32(t, 0); put32(t, 0);
      put64(t, count); put64(t, count); put64(t, cd_size); put64(t, cd_off);
      put32(t, 0x07064b50u); put32(t, 0); put64(t, z_off); put32(t, 1);
    }
    put32(t, 0x06054b50u);
    put16(t, 0); put16(t, 0);
    put16(t, big ? 0xFFFF : (uint32_t)count); put16(t, big ? 0xFFFF : (uint32_t)count);
    put32(t, big ? 0xFFFFFFFFu : (uint32_t)cd_size);
    put32(t, big ? 0xFFFFFFFFu : (uint32_t)cd_off);
    put16(t, 0);
    if (!raw(t.data(), t.size())) return false;
    f_.flush();
    return (bool)f_;
  }
  bool close() { f_.close(); return !f_.fail(); }
  const std::vector<ZEntry>& entries() const { return es_; }

 private:
  bool raw(const void* p, size_t n) {
    f_.write((const char*)p, (std::streamsize)n);
    off_ += n;
    return (bool)f_;
  }
  std::ofstream f_;
  std::vector<char> buf_;
  uint64_t off_ = 0, written_ = 0;
  uint32_t crc_ = 0;
  ZEntry cur_;
  std::vector<ZEntry> es_;
};

// Forces the finished file to disk (a sync client reads it right after the rename).
void flush_to_disk(const fs::path& p) {
#ifdef _WIN32
  HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) { FlushFileBuffers(h); CloseHandle(h); }
#else
  (void)p;
#endif
}

// ---- zip reading (our own archives: stored entries) -------------------------------------------------------------------------------------

constexpr uint64_t kBig = 1ull << 50;   // no size cap when listing a backup

struct ZipIn {
  std::ifstream f;
  bool open(const fs::path& p) { f.open(p, std::ios::binary); return (bool)f; }
  // Streams a stored entry's bytes to fn (return false to stop). False when the zip is damaged or the entry is not stored.
  bool stream(const zipr::Entry& e, const std::function<bool(const uint8_t*, size_t)>& fn) {
    if (e.method != 0 || e.csize != e.size) return false;
    uint8_t lh[30];
    f.clear();
    f.seekg((std::streamoff)e.lho);
    f.read((char*)lh, 30);
    if (f.gcount() != 30 || lh[0] != 0x50 || lh[1] != 0x4b || lh[2] != 3 || lh[3] != 4) return false;
    const size_t nl = (size_t)(lh[26] | (lh[27] << 8)), xl = (size_t)(lh[28] | (lh[29] << 8));
    std::string lname(nl, '\0');   // the local header must name the same file as the directory entry
    f.read(lname.data(), (std::streamsize)nl);
    if (f.gcount() != (std::streamsize)nl || lname != e.rel) return false;
    f.seekg((std::streamoff)(e.lho + 30 + nl + xl));
    std::vector<uint8_t> b(1 << 20);
    uint64_t left = e.size;
    while (left) {
      const size_t n = (size_t)std::min<uint64_t>(left, b.size());
      f.read((char*)b.data(), (std::streamsize)n);
      if ((size_t)f.gcount() != n) return false;
      if (!fn(b.data(), n)) return false;
      left -= n;
    }
    return true;
  }
};

struct ManFile { std::string path; uint64_t size = 0; std::string sha; };
struct Manifest {
  int format = 0;
  std::string profile_id, profile_name, signature, summary, created_text;
  int64_t created = 0;
  bool saves = false, settings = false, photos = false;
  std::vector<std::pair<std::string, uint64_t>> gens;
  std::vector<ManFile> files;
};

bool safe_rel(const std::string& n) {
  if (n.empty() || n[0] == '/' || n.find('\\') != std::string::npos || n.find(':') != std::string::npos) return false;
  size_t i = 0;
  while (i <= n.size()) {
    size_t j = n.find('/', i);
    if (j == std::string::npos) j = n.size();
    const std::string part = n.substr(i, j - i);
    if (part.empty() || part == "." || part == "..") return false;
    i = j + 1;
  }
  return true;
}

bool parse_manifest(const std::string& text, Manifest* m, std::string* err) {
  Json j;
  std::string e;
  JsonParser jp(text);
  if (!jp.parse(j, e) || j.type != Json::Obj) { *err = "The manifest cannot be read (" + e + ")."; return false; }
  const Json* fm = j.get("format");
  m->format = fm ? (int)fm->num() : 0;
  if (m->format != 1) { *err = "The manifest is from a version this one does not read (format " + std::to_string(m->format) + ")."; return false; }
  if (const Json* x = j.get("profile_id")) m->profile_id = x->str();
  if (const Json* x = j.get("profile_name")) m->profile_name = x->str();
  if (const Json* x = j.get("created")) m->created = (int64_t)x->num();
  if (const Json* x = j.get("created_text")) m->created_text = x->str();
  if (const Json* x = j.get("summary")) m->summary = x->str();
  if (const Json* x = j.get("signature")) m->signature = x->str();
  if (const Json* x = j.get("saves")) m->saves = x->boolean();
  if (const Json* x = j.get("settings")) m->settings = x->boolean();
  if (const Json* x = j.get("photos")) m->photos = x->boolean();
  if (const Json* g = j.get("generations")) if (g->type == Json::Arr)
    for (const Json& q : g->a) {
      const Json* u = q.get("uid");
      const Json* n = q.get("gen");
      if (u && n) m->gens.push_back({u->str(), (uint64_t)n->num()});
    }
  const Json* fl = j.get("files");
  if (!fl || fl->type != Json::Arr) { *err = "The manifest has no file list."; return false; }
  for (const Json& q : fl->a) {
    ManFile f;
    const Json* p = q.get("path");
    const Json* s = q.get("size");
    const Json* h = q.get("sha256");
    if (!p || !s || !h || p->type != Json::Str || h->type != Json::Str) { *err = "The manifest has a bad file entry."; return false; }
    f.path = p->s;
    f.size = (uint64_t)s->num();
    f.sha = h->s;
    if (!safe_rel(f.path)) { *err = "The manifest names an unsafe path: " + f.path; return false; }
    m->files.push_back(f);
  }
  return true;
}

// Lists the zip and reads its manifest (the last entry, checked against its CRC).
bool open_manifest(const fs::path& zip, std::vector<zipr::Entry>* ents, Manifest* m, std::string* err) {
  if (!zipr::list(zip, *ents, err, kBig)) return false;
  const zipr::Entry* me = nullptr;
  for (const zipr::Entry& e : *ents) if (e.rel == "manifest.json") me = &e;
  if (!me) { *err = "The zip has no manifest.json (it was not finished)."; return false; }
  if (me->size > (256ull << 20)) { *err = "The manifest is too large."; return false; }
  ZipIn zi;
  if (!zi.open(zip)) { *err = "The zip cannot be opened."; return false; }
  std::string text;
  uint32_t crc = 0;
  if (!zi.stream(*me, [&](const uint8_t* p, size_t n) { text.append((const char*)p, n); crc = zipr::crc32_update(crc, p, n); return true; })) { *err = "The manifest cannot be read."; return false; }
  if (crc != me->crc) { *err = "The manifest does not match its checksum."; return false; }
  return parse_manifest(text, m, err);
}

using Want = std::function<bool(const std::string&)>;

// Hashes every wanted manifest file in the zip (others are only checked to exist) and checks the zip holds nothing the manifest does not list.
bool verify_impl(const fs::path& zip, const Want& want, Manifest* out, std::string* err) {
  std::vector<zipr::Entry> ents;
  Manifest m;
  if (!open_manifest(zip, &ents, &m, err)) return false;
  std::map<std::string, const zipr::Entry*> by;
  for (const zipr::Entry& e : ents) {
    if (e.rel == "manifest.json") continue;
    if (!by.emplace(e.rel, &e).second) { *err = "The zip lists " + e.rel + " twice."; return false; }
  }
  std::set<std::string> seen;
  ZipIn zi;
  if (!zi.open(zip)) { *err = "The zip cannot be opened."; return false; }
  for (const ManFile& f : m.files) {
    if (!seen.insert(f.path).second) { *err = "The manifest lists " + f.path + " twice."; return false; }
    auto it = by.find(f.path);
    if (it == by.end()) { *err = "The zip is missing " + f.path + "."; return false; }
    const zipr::Entry& e = *it->second;
    if (e.size != f.size) { *err = f.path + " has the wrong size (the manifest says " + std::to_string(f.size) + ", the zip holds " + std::to_string(e.size) + ")."; return false; }
    if (want && !want(f.path)) continue;
    Sha sha;
    uint32_t crc = 0;
    if (!zi.stream(e, [&](const uint8_t* p, size_t n) { sha.update(p, n); crc = zipr::crc32_update(crc, p, n); return true; })) { *err = f.path + " cannot be read from the zip."; return false; }
    if (crc != e.crc) { *err = f.path + " does not match the zip's own checksum."; return false; }
    if (sha.hex() != f.sha) { *err = f.path + " does not match its SHA-256 in the manifest."; return false; }
  }
  for (const auto& kv : by) if (!seen.count(kv.first)) { *err = "The zip holds " + kv.first + ", which the manifest does not list."; return false; }
  if (out) *out = m;
  return true;
}

// Copies one stored entry out of the zip, checking its SHA-256. The file is written to dst.part and renamed, so dst is never half written.
bool extract_checked(ZipIn& zi, const zipr::Entry& e, const std::string& sha, const fs::path& dst, std::string* err) {
  std::error_code ec;
  if (dst.has_parent_path()) fs::create_directories(dst.parent_path(), ec);
  const fs::path tmp = dst.string() + ".part";
  Sha h;
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    if (!o) { *err = "Could not write " + S(dst); return false; }
    const bool ok = zi.stream(e, [&](const uint8_t* p, size_t n) { h.update(p, n); o.write((const char*)p, (std::streamsize)n); return (bool)o; });
    o.flush();
    if (!ok || !o) { o.close(); fs::remove(tmp, ec); *err = "Could not copy " + e.rel + " (the zip is damaged or the disk is full)."; return false; }
  }
  if (h.hex() != sha) { fs::remove(tmp, ec); *err = e.rel + " does not match its SHA-256."; return false; }
  if (!replace_file(tmp, dst)) { fs::remove(tmp, ec); *err = "Could not finish " + S(dst); return false; }
  return true;
}

}  // namespace

// ---- names ------------------------------------------------------------------------------------------------------------------------------

static std::string id_part(const std::string& id) {
  std::string r;
  for (unsigned char c : id) r.push_back(std::isalnum(c) || c == '_' ? (char)c : '_');
  return r.empty() ? "default" : r;
}

static std::string stamp_of(int64_t t) {
  std::time_t tt = (std::time_t)t;
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  char b[32];
  std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
  return b;
}

static bool parse_stamp(const std::string& n, int64_t* when) {
  if (n.size() != 15 || n[8] != '-') return false;
  for (int i = 0; i < 15; ++i) if (i != 8 && !std::isdigit((unsigned char)n[(size_t)i])) return false;
  std::tm t{};
  t.tm_year = std::atoi(n.substr(0, 4).c_str()) - 1900;
  t.tm_mon = std::atoi(n.substr(4, 2).c_str()) - 1;
  t.tm_mday = std::atoi(n.substr(6, 2).c_str());
  t.tm_hour = std::atoi(n.substr(9, 2).c_str());
  t.tm_min = std::atoi(n.substr(11, 2).c_str());
  t.tm_sec = std::atoi(n.substr(13, 2).c_str());
  t.tm_isdst = -1;
  if (when) *when = (int64_t)std::mktime(&t);
  return true;
}

// "SnapRecomp-<id>-<YYYYMMDD-HHMMSS>.zip" -> id, when
static bool parse_name(const std::string& n, std::string* id, int64_t* when) {
  const std::string pre = "SnapRecomp-", post = ".zip";
  if (n.size() < pre.size() + 1 + 16 + post.size() || n.compare(0, pre.size(), pre) != 0 || n.compare(n.size() - post.size(), post.size(), post) != 0) return false;
  const std::string mid = n.substr(pre.size(), n.size() - pre.size() - post.size());
  if (mid.size() < 17 || mid[mid.size() - 16] != '-') return false;
  if (!parse_stamp(mid.substr(mid.size() - 15), when)) return false;
  if (id) *id = mid.substr(0, mid.size() - 16);
  return true;
}

static std::string human(uint64_t b) {
  char t[40];
  if (b >= (1ull << 30)) std::snprintf(t, sizeof t, "%.1f GB", (double)b / (1ull << 30));
  else if (b >= (1ull << 20)) std::snprintf(t, sizeof t, "%.1f MB", (double)b / (1ull << 20));
  else std::snprintf(t, sizeof t, "%.1f KB", (double)b / 1024.0);
  return t;
}

void remove_leftovers(const std::string& folder) {
  std::error_code ec;
  std::vector<fs::path> dead;
  for (fs::directory_iterator it(P(folder), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = S(it->path().filename());
    if (n.compare(0, 11, "SnapRecomp-") == 0 && n.size() > 9 && n.compare(n.size() - 9, 9, ".zip.part") == 0) dead.push_back(it->path());
  }
  for (const fs::path& p : dead) fs::remove(p, ec);   // a part another run still has open cannot be deleted: it stays
}

std::vector<Backup> list_backups(const std::string& folder, const std::string& profile_id) {
  std::vector<Backup> out;
  std::error_code ec;
  for (fs::directory_iterator it(P(folder), ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2)) continue;
    Backup b;
    b.file = S(it->path().filename());
    if (!parse_name(b.file, &b.profile_id, &b.when)) continue;
    b.path = S(it->path());
    b.bytes = (uint64_t)it->file_size(e2);
    std::vector<zipr::Entry> ents;
    Manifest m;
    std::string err;
    if (open_manifest(it->path(), &ents, &m, &err)) {
      b.valid = true;
      if (!m.profile_id.empty()) b.profile_id = m.profile_id;
      b.profile_name = m.profile_name;
      b.summary = m.summary;
      b.signature = m.signature;
      b.has_saves = m.saves; b.has_settings = m.settings; b.has_photos = m.photos;
    } else {
      b.problem = err;
      b.profile_name = b.profile_id;
    }
    if (!profile_id.empty() && b.profile_id != profile_id && id_part(b.profile_id) != id_part(profile_id)) continue;
    out.push_back(b);
  }
  auto stamp = [](const Backup& x) { return x.file.substr(x.file.size() - 19); };   // "YYYYMMDD-HHMMSS.zip"
  std::sort(out.begin(), out.end(), [&](const Backup& a, const Backup& b) { return stamp(a) != stamp(b) ? stamp(a) > stamp(b) : a.file > b.file; });
  return out;
}

std::string describe(const Backup& b) {
  std::string s = profiles::format_time(b.when) + "  " + human(b.bytes) + "  (profile " + (b.profile_name.empty() ? b.profile_id : b.profile_name) + ")";
  if (!b.valid) s += "  DAMAGED: " + b.problem;
  return s;
}

bool verify_zip(const std::string& zip, std::string* err) {
  std::string e;
  const bool ok = verify_impl(P(zip), Want(), nullptr, &e);
  if (!ok && err) *err = e;
  return ok;
}

// ---- backup ----------------------------------------------------------------------------------------------------------------------------

namespace {

struct Item {
  std::string name;
  fs::path path;
  std::string mem;
  bool is_mem = false, optional = false;
  uint64_t size = 0;
};

void add_tree(std::vector<Item>& items, const fs::path& dir, const std::string& prefix, bool optional, std::vector<std::string>* notes) {
  std::error_code ec;
  std::vector<std::pair<std::string, fs::path>> found;
  for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2)) continue;
    found.push_back({prefix + G(it->path().lexically_relative(dir)), it->path()});
  }
  if (ec && notes) notes->push_back("Part of " + S(dir) + " could not be read: " + ec.message());
  std::sort(found.begin(), found.end());
  for (auto& f : found) {
    Item i;
    i.name = f.first;
    i.path = f.second;
    i.optional = optional;
    std::error_code e3;
    i.size = (uint64_t)fs::file_size(f.second, e3);
    if (e3) { if (notes) notes->push_back("Left out (unreadable): " + S(f.second)); continue; }
    items.push_back(std::move(i));
  }
}

void write_last_file(const std::string& dir, const char* status, const std::string& msg) {
  if (dir.empty()) return;
  write_text_atomic(P(dir) / "backup_last.txt", std::string(status) + "\n" + std::to_string((long long)profiles::now_seconds()) + "\n" + msg + "\n");
}

}  // namespace

Last last_result(const std::string& dir) {
  Last l;
  std::string t;
  if (!read_text(P(dir) / "backup_last.txt", &t)) return l;
  size_t a = t.find('\n');
  if (a == std::string::npos) return l;
  l.status = t.substr(0, a);
  size_t b = t.find('\n', a + 1);
  if (b == std::string::npos) return l;
  l.when = std::atoll(t.substr(a + 1, b - a - 1).c_str());
  l.message = t.substr(b + 1);
  while (!l.message.empty() && (l.message.back() == '\n' || l.message.back() == '\r')) l.message.pop_back();
  return l;
}

RunResult run_backup(const Source& src, const Settings& set, bool force) {
  RunResult R;
  auto fail = [&](const std::string& m) { R.outcome = Outcome::Failed; R.message = m; return R; };
  if (set.folder.empty()) return fail("No backup folder is chosen.");
  if (!set.saves && !set.settings && !set.photos) return fail("Nothing is selected to back up.");
  const fs::path folder = P(set.folder);
  std::error_code ec;
  fs::create_directories(folder, ec);
  if (ec || !fs::is_directory(folder, ec)) return fail("The backup folder cannot be used: " + set.folder);

  // Never back up a save that is being written, or one the game has to repair first.
  SaveState st;
  if (set.saves) {
    st = inspect_save(src.save_root);
    if (st.health == Health::InUse || st.health == Health::MidWrite || st.health == Health::NeedsRecovery) {
      R.outcome = Outcome::Skipped;
      R.message = std::string(st.health == Health::NeedsRecovery ? "Not backed up: the save is waiting for recovery. " : st.health == Health::MidWrite ? "Not backed up: the save was caught mid-write. " : "Not backed up: the game is using the save. ") + st.detail;
      return R;
    }
    if (st.health == Health::NoSave && !set.settings && !set.photos) { R.outcome = Outcome::Unchanged; R.message = "There is no save to back up yet."; return R; }
    if (!src.profiles_root.empty()) {
      profiles::Store ps(src.profiles_root);
      ps.load();
      if (ps.is_pending(src.profile_id)) { R.outcome = Outcome::Skipped; R.message = "Not backed up: a clear or delete is queued for this profile."; return R; }
    }
  }
  remove_leftovers(set.folder);

  if (!force) {   // the save did not change since the newest copy of this profile: nothing to do
    const auto have = list_backups(set.folder, src.profile_id);
    for (const Backup& b : have) {
      if (!b.valid) continue;
      const bool same_inc = b.has_saves == set.saves && b.has_settings == set.settings && b.has_photos == set.photos;
      if (set.saves && same_inc && b.signature == st.signature() && !st.gens.empty()) { R.outcome = Outcome::Unchanged; R.message = "The save has not changed since the last backup (" + profiles::format_time(b.when) + ")."; R.zip = b.path; return R; }
      break;
    }
  }

  // The file list
  std::vector<Item> items;
  std::vector<std::string> notes;
  if (set.saves) {
    for (const auto& g : st.gens) {
      const fs::path ud = P(src.save_root) / P(g.first);
      const std::string base = "save/" + g.first + "/";
      Item cur;
      cur.name = base + "CURRENT"; cur.is_mem = true; cur.mem = sg::pointer_text(g.second); cur.size = cur.mem.size();
      items.push_back(cur);
      Item ok;
      ok.name = base + "gens/" + std::to_string(g.second) + ".ok"; ok.path = sg::gen_ok(ud, g.second);
      ok.size = (uint64_t)fs::file_size(ok.path, ec);
      items.push_back(ok);
      add_tree(items, sg::gen_dir(ud, g.second), base + "gens/" + std::to_string(g.second) + "/", false, &notes);
    }
  }
  if (set.settings) {
    std::vector<std::pair<std::string, std::string>> cands;
    if (!src.profile_cfg.empty()) cands.push_back({"settings/profile/settings.cfg", src.profile_cfg});
    for (const auto& g : src.globals) cands.push_back({"settings/global/" + g.first, g.second});
    for (const auto& c : cands) {
      std::error_code e2;
      if (!fs::is_regular_file(P(c.second), e2)) continue;
      Item i; i.name = c.first; i.path = P(c.second); i.optional = true; i.size = (uint64_t)fs::file_size(i.path, e2);
      if (!e2) items.push_back(i);
    }
  }
  if (set.photos) for (const auto& d : src.photo_dirs) add_tree(items, P(d.second), "photos/" + d.first + "/", true, &notes);
  uint64_t total = 0;
  for (const Item& i : items) total += i.size;
  {
    std::error_code e2;
    const auto sp = fs::space(folder, e2);
    if (!e2 && sp.available < total + (total / 64) + (4u << 20)) return fail("Not enough free space in the backup folder (" + human(total) + " needed, " + human(sp.available) + " free).");
  }

  const int64_t now = profiles::now_seconds();
  int64_t when = now;
  std::string name;
  for (int tries = 0; tries < 20; ++tries, ++when) {
    name = "SnapRecomp-" + id_part(src.profile_id) + "-" + stamp_of(when) + ".zip";
    if (!fs::exists(folder / name, ec)) break;
  }
  const fs::path fin = folder / name, part = folder / (name + ".part");
  fs::remove(part, ec);

  ZipWriter zw;
  if (!zw.open(part)) return fail("Cannot write " + S(part));
  auto drop = [&](const std::string& m) { zw.close(); std::error_code e2; fs::remove(part, e2); return fail(m); };
  std::vector<ManFile> mf;
  std::vector<uint8_t> buf(1 << 20);
  uint64_t save_bytes = 0, photo_bytes = 0;
  int save_files = 0, photo_files = 0;
  for (const Item& it : items) {
    Sha sha;
    std::ifstream in;
    if (!it.is_mem) {
      in.open(it.path, std::ios::binary);
      if (!in) {
        if (it.optional) { notes.push_back("Left out (could not open): " + S(it.path)); continue; }
        return drop("Cannot read " + S(it.path) + " (is the game running?).");
      }
    }
    if (!zw.begin(it.name, it.size, now)) return drop("Writing the backup failed (disk full?).");
    if (it.is_mem) {
      sha.update(it.mem.data(), it.mem.size());
      if (!zw.write((const uint8_t*)it.mem.data(), it.mem.size())) return drop("Writing the backup failed (disk full?).");
    } else {
      uint64_t left = it.size;
      while (left) {
        const size_t n = (size_t)std::min<uint64_t>(left, buf.size());
        in.read((char*)buf.data(), (std::streamsize)n);
        if ((size_t)in.gcount() != n) return drop("A file changed while it was being backed up: " + S(it.path) + ". Try again.");
        sha.update(buf.data(), n);
        if (!zw.write(buf.data(), n)) return drop("Writing the backup failed (disk full?).");
        left -= n;
      }
      char probe;
      if (in.read(&probe, 1).gcount() != 0) return drop("A file changed while it was being backed up: " + S(it.path) + ". Try again.");
    }
    if (!zw.end()) return drop("Writing the backup failed.");
    ManFile f; f.path = it.name; f.size = it.size; f.sha = sha.hex();
    mf.push_back(f);
    if (it.name.compare(0, 5, "save/") == 0 && it.name.find("/gens/") != std::string::npos && it.name.compare(it.name.size() - 3, 3, ".ok") != 0) { save_bytes += it.size; ++save_files; }   // the game's own files, not the pointer and marker
    else if (it.name.compare(0, 7, "photos/") == 0) { photo_bytes += it.size; ++photo_files; }
  }
  if (set.saves) {   // the save must be the one we described: a game that started meanwhile ends the attempt
    const SaveState again = inspect_save(src.save_root);
    if (again.health != Health::Ok || again.signature() != st.signature()) return drop("The save changed while it was being backed up. Try again.");
  }

  std::string summary;
  if (set.saves) summary += "save: " + std::to_string(save_files) + " files, " + human(save_bytes);
  if (set.settings) { bool any = false; for (const ManFile& f : mf) if (f.path.compare(0, 9, "settings/") == 0) any = true; if (any) summary += std::string(summary.empty() ? "" : "; ") + "settings"; }
  if (set.photos) summary += std::string(summary.empty() ? "" : "; ") + "photos: " + std::to_string(photo_files) + " files, " + human(photo_bytes);
  Json j = Json::object();
  j.set("format", Json::number(1));
  j.set("app", Json::string("SnapRecomp"));
  j.set("created", Json::number((double)now));
  j.set("created_text", Json::string(profiles::format_time(now)));
  j.set("profile_id", Json::string(src.profile_id));
  j.set("profile_name", Json::string(src.profile_name));
  j.set("saves", Json::boolean_(set.saves));
  j.set("settings", Json::boolean_(set.settings));
  j.set("photos", Json::boolean_(set.photos));
  j.set("signature", Json::string(set.saves ? st.signature() : ""));
  j.set("summary", Json::string(summary));
  Json gens = Json::array();
  if (set.saves) for (const auto& g : st.gens) { Json q = Json::object(); q.set("uid", Json::string(g.first)); q.set("gen", Json::number((double)g.second)); gens.a.push_back(q); }
  j.set("generations", gens);
  Json notes_j = Json::array();
  for (const std::string& n : notes) notes_j.a.push_back(Json::string(n));
  j.set("notes", notes_j);
  Json files = Json::array();
  for (const ManFile& f : mf) { Json q = Json::object(); q.set("path", Json::string(f.path)); q.set("size", Json::number((double)f.size)); q.set("sha256", Json::string(f.sha)); files.a.push_back(q); }
  j.set("files", files);
  std::string text;
  json_dump(j, text);
  text += "\n";
  if (!zw.add_bytes("manifest.json", text, now) || !zw.finish() || !zw.close()) return drop("Writing the backup failed (disk full?).");   // the manifest goes in last
  flush_to_disk(part);
  if (g_corrupt_part) {   // test hook: a bad write the verification has to catch
    std::fstream cf(part, std::ios::in | std::ios::out | std::ios::binary);
    char c = 0;
    const std::streamoff at = (std::streamoff)(fs::file_size(part, ec) / 2);
    cf.seekg(at); cf.read(&c, 1); c = (char)(c ^ 0x5a); cf.seekp(at); cf.write(&c, 1);
  }

  std::string verr;
  if (!verify_zip(S(part), &verr)) { std::error_code e2; fs::remove(part, e2); return fail("The new backup did not verify, so it was thrown away: " + verr); }
  if (!replace_file(part, fin)) { std::error_code e2; fs::remove(part, e2); return fail("Could not finish " + name + " in the backup folder."); }
  R.zip = S(fin);
  R.bytes = (uint64_t)fs::file_size(fin, ec);
  R.files = (int)mf.size();
  R.notes = notes;

  // Only now, with the new copy verified and in place, the oldest ones go.
  int kept = 0;
  for (const Backup& b : list_backups(set.folder, src.profile_id)) {
    if (!b.valid) continue;   // a damaged zip is never counted and never deleted here
    if (++kept <= std::max(1, set.keep)) continue;
    std::error_code e2;
    if (fs::remove(P(b.path), e2)) ++R.removed;
  }
  R.outcome = Outcome::Made;
  R.message = "Backed up " + std::to_string(R.files) + " files (" + human(R.bytes) + ") to " + S(fin.filename()) + (R.removed ? "; removed " + std::to_string(R.removed) + " older cop" + (R.removed == 1 ? "y" : "ies") : "") + ".";
  return R;
}

void write_last(const std::string& dir, const RunResult& r) {
  const char* s = r.outcome == Outcome::Made ? "made" : r.outcome == Outcome::Unchanged ? "unchanged" : r.outcome == Outcome::Skipped ? "warning" : "failed";
  write_last_file(dir, s, r.message);
}

// ---- restore ---------------------------------------------------------------------------------------------------------------------------

namespace {

std::string new_profile_name(const profiles::Store& s, const std::string& base, int64_t when) {
  std::string stem = base;
  if (stem.size() > 18) stem.resize(18);
  const std::string t = profiles::format_time(when).substr(0, 10);
  std::string name = stem + " (backup " + t + ")";
  for (int n = 2; s.find(name) && n < 100; ++n) name = stem + " (backup " + t + " " + std::to_string(n) + ")";
  return name;
}

bool want_restore(const std::string& p, const RestoreOptions& o) {
  if (p.compare(0, 5, "save/") == 0) return true;
  if (p == "settings/profile/settings.cfg") return o.settings;
  if (p.compare(0, 7, "photos/") == 0) return o.photos;
  return false;
}

// Extracts the generation `uid`/`gen` of the backup into <tmp>/ (the layout import_save reads: the files at the top) and imports it into `save_root`.
bool import_generation(ZipIn& zi, const std::map<std::string, const zipr::Entry*>& by, const Manifest& m, const std::string& uid, uint64_t gen, const fs::path& tmp,
                       const std::string& save_root, RestoreReport* rep, std::string* err, bool* running) {
  const std::string pre = "save/" + uid + "/gens/" + std::to_string(gen) + "/";
  std::error_code ec;
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);
  int n = 0;
  for (const ManFile& f : m.files) {
    if (f.path.compare(0, pre.size(), pre) != 0) continue;
    auto it = by.find(f.path);
    if (it == by.end() || !extract_checked(zi, *it->second, f.sha, tmp / P(f.path.substr(pre.size())), err)) { fs::remove_all(tmp, ec); if (err->empty()) *err = "The zip is missing " + f.path; return false; }
    ++n;
  }
  if (!n) { *err = "The backup holds no save files for " + uid + "."; return false; }
  saveport::Report r;
  std::string ie;
  bool ok = saveport::import_save(S(tmp), save_root, uid, &r, &ie, running);
  fs::remove_all(tmp, ec);
  if (!ok) { *err = ie; return false; }
  rep->save = r;
  return true;
}

// Restores the optional parts (settings.cfg, missing photos) of a verified backup. Never overwrites a photo.
void restore_extras(ZipIn& zi, const std::map<std::string, const zipr::Entry*>& by, const Manifest& m, const Source& cur, const RestoreOptions& opt, const fs::path& cfg_dest,
                    RestoreReport* rep) {
  std::string err;
  for (const ManFile& f : m.files) {
    auto it = by.find(f.path);
    if (it == by.end()) continue;
    if (f.path == "settings/profile/settings.cfg" && opt.settings && !cfg_dest.empty()) {
      std::error_code ec;
      if (fs::exists(cfg_dest, ec)) fs::copy_file(cfg_dest, cfg_dest.string() + ".bak-restore", fs::copy_options::overwrite_existing, ec);
      if (extract_checked(zi, *it->second, f.sha, cfg_dest, &err)) rep->settings_restored = true;
      else rep->notes.push_back("settings.cfg was not restored: " + err);
    } else if (f.path.compare(0, 7, "photos/") == 0 && opt.photos) {
      const size_t slash = f.path.find('/', 7);
      if (slash == std::string::npos) continue;
      const std::string label = f.path.substr(7, slash - 7);
      std::string dir;
      for (const auto& d : cur.photo_dirs) if (d.first == label) dir = d.second;
      if (dir.empty()) continue;
      const fs::path dst = P(dir) / P(f.path.substr(slash + 1));
      std::error_code ec;
      if (fs::exists(dst, ec)) { rep->photos_kept++; continue; }
      if (extract_checked(zi, *it->second, f.sha, dst, &err)) rep->photos_restored++;
      else rep->notes.push_back("A photo was not restored: " + err);
    }
  }
}

}  // namespace

bool restore_new_profile(const std::string& zip, const Source& cur, const RestoreOptions& opt, RestoreReport* repp, std::string* errp) {
  RestoreReport local;
  RestoreReport& rep = repp ? *repp : local;
  rep = RestoreReport();
  std::string e2;
  std::string& err = errp ? *errp : e2;
  err.clear();
  if (cur.profiles_root.empty()) { err = "Profiles are off here, so there is no way to make a new profile. Turn profiles on, or restore over the current save."; return false; }
  Manifest m;
  const Want want = [&](const std::string& p) { return want_restore(p, opt); };
  if (!verify_impl(P(zip), want, &m, &err)) { err = "That backup did not verify, so nothing was restored. " + err; return false; }
  if (!m.saves || m.gens.empty()) { err = "That backup holds no save."; return false; }
  std::vector<zipr::Entry> ents;
  Manifest m2;
  if (!open_manifest(P(zip), &ents, &m2, &err)) return false;
  std::map<std::string, const zipr::Entry*> by;
  for (const zipr::Entry& e : ents) by[e.rel] = &e;

  profiles::Store st(cur.profiles_root);
  st.load();
  const std::string name = new_profile_name(st, m.profile_name.empty() ? m.profile_id : m.profile_name, m.created ? m.created : profiles::now_seconds());
  std::string perr;
  const std::string id = st.create(name, &perr);
  if (id.empty()) { err = "The new profile could not be made: " + perr; return false; }
  rep.profile_id = id;
  rep.profile_name = name;
  auto undo = [&]() { std::string x; st.remove(id, &x); rep.profile_id.clear(); rep.profile_name.clear(); };
  ZipIn zi;
  if (!zi.open(P(zip))) { err = "The zip cannot be opened."; undo(); return false; }
  const fs::path tmp = P(st.save_dir(id)).parent_path() / ".restore-tmp";
  for (const auto& g : m.gens) {
    bool running = false;
    if (!import_generation(zi, by, m, g.first, g.second, tmp, st.save_dir(id), &rep, &err, &running)) { undo(); return false; }
  }
  restore_extras(zi, by, m, cur, opt, P(cur.profiles_root) / P(id) / "settings.cfg", &rep);
  return true;
}

bool restore_replace_current(const std::string& zip, const Source& cur, const RestoreOptions& opt, bool confirmed, RestoreReport* repp, std::string* errp, bool* refused_running) {
  RestoreReport local;
  RestoreReport& rep = repp ? *repp : local;
  rep = RestoreReport();
  std::string e2;
  std::string& err = errp ? *errp : e2;
  err.clear();
  if (refused_running) *refused_running = false;
  if (!confirmed) { err = "Replacing the current profile needs a second confirmation. Nothing was changed."; return false; }
  if (profiles::save_in_use(cur.save_root)) { if (refused_running) *refused_running = true; err = "The game is running (it holds this save folder). Close it first. Nothing was changed."; return false; }
  Manifest m;
  const Want want = [&](const std::string& p) { return want_restore(p, opt); };
  if (!verify_impl(P(zip), want, &m, &err)) { err = "That backup did not verify, so nothing was restored. " + err; return false; }
  if (!m.saves || m.gens.empty()) { err = "That backup holds no save."; return false; }
  std::vector<zipr::Entry> ents;
  Manifest m2;
  if (!open_manifest(P(zip), &ents, &m2, &err)) return false;
  std::map<std::string, const zipr::Entry*> by;
  for (const zipr::Entry& e : ents) by[e.rel] = &e;
  std::string made, be;
  if (!savebackup::make(cur.save_root, "restore", &made, &be, std::max(1, savebackup::keep_count()))) { err = "The current save could not be backed up first (" + be + "), so nothing was changed."; return false; }
  ZipIn zi;
  if (!zi.open(P(zip))) { err = "The zip cannot be opened."; return false; }
  const fs::path tmp = P(cur.save_root).parent_path() / ".restore-tmp";
  for (const auto& g : m.gens) {
    bool running = false;
    if (!import_generation(zi, by, m, g.first, g.second, tmp, cur.save_root, &rep, &err, &running)) { if (refused_running) *refused_running = running; return false; }
  }
  rep.profile_id = cur.profile_id;
  rep.profile_name = cur.profile_name;
  restore_extras(zi, by, m, cur, opt, cur.profile_cfg.empty() ? fs::path() : P(cur.profile_cfg), &rep);
  return true;
}

}  // namespace syncbackup
}  // namespace hle
