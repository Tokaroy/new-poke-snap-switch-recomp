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
#include "save_backup.h"
#include "save_gen.h"
#include "zip_read.h"
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
// Zip reading lives in zip_read.cpp.
namespace {

using hle::zipr::crc32_update;

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


int64_t to_unix(fs::file_time_type t) {
  using namespace std::chrono;
  return (int64_t)duration_cast<seconds>(clock_cast<system_clock>(t).time_since_epoch()).count();
}

bool list_zip(const fs::path& zp, Source& src, std::string* err) {
  std::vector<hle::zipr::Entry> v;
  if (!hle::zipr::list(zp, v, err)) return false;
  for (const auto& z : v) {
    SrcEntry se;
    se.rel = z.rel; se.size = z.size; se.csize = z.csize; se.lho = z.lho; se.method = z.method; se.crc = z.crc;
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
  hle::zipr::Entry z;
  z.rel = e.rel; z.size = e.size; z.csize = e.csize; z.lho = e.lho; z.method = e.method; z.crc = e.crc;
  return hle::zipr::extract(src.path, z, dst, err);
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

// Writes the files as a zip with the files at the top level, the layout JKSV makes and restores (stored, no compression: the
// reader of a JKSV zip needs nothing more, and every size stays under 4 GB). CRCs follow the data (flag bit 3).
static bool write_zip(const fs::path& gd, const std::vector<std::pair<std::string, uint64_t>>& files, const fs::path& zip, std::string* E) {
  uint64_t total = 0;
  for (auto& f : files) total += f.second + 200;
  if (total >= 0xFFFF0000ull) { *E = "The save is too large for a zip file (over 4 GB). Export to a folder instead."; return false; }
  std::error_code ec;
  if (zip.has_parent_path()) fs::create_directories(zip.parent_path(), ec);
  const fs::path tmp = zip.string() + ".part";
  std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
  if (!o) { *E = "Could not write " + S(zip); return false; }
  auto w16 = [&](uint32_t v) { char b[2] = {(char)v, (char)(v >> 8)}; o.write(b, 2); };
  auto w32 = [&](uint32_t v) { char b[4] = {(char)v, (char)(v >> 8), (char)(v >> 16), (char)(v >> 24)}; o.write(b, 4); };
  struct CD { std::string name; uint32_t crc, size, off; };
  std::vector<CD> cd;
  const uint32_t dos_time = 0, dos_date = (uint32_t)(((2026 - 1980) << 9) | (1 << 5) | 1);
  std::vector<char> buf(1 << 20);
  for (auto& [name, sz] : files) {
    CD c{name, 0, (uint32_t)sz, (uint32_t)o.tellp()};
    w32(0x04034b50); w16(20); w16(8); w16(0); w16(dos_time); w16(dos_date); w32(0); w32(0); w32(0); w16((uint32_t)name.size()); w16(0);
    o.write(name.data(), (std::streamsize)name.size());
    std::ifstream in(gd / name, std::ios::binary);
    uint64_t left = sz;
    while (left) {
      size_t n = (size_t)std::min<uint64_t>(left, buf.size());
      in.read(buf.data(), (std::streamsize)n);
      if ((size_t)in.gcount() != n) { *E = "Could not read " + name + " from the save."; return false; }
      o.write(buf.data(), (std::streamsize)n);
      c.crc = crc32_update(c.crc, (const uint8_t*)buf.data(), n);
      left -= n;
    }
    w32(0x08074b50); w32(c.crc); w32(c.size); w32(c.size);
    cd.push_back(c);
  }
  const uint32_t cd_off = (uint32_t)o.tellp();
  for (auto& c : cd) {
    w32(0x02014b50); w16(20); w16(20); w16(8); w16(0); w16(dos_time); w16(dos_date); w32(c.crc); w32(c.size); w32(c.size);
    w16((uint32_t)c.name.size()); w16(0); w16(0); w16(0); w16(0); w32(0); w32(c.off);
    o.write(c.name.data(), (std::streamsize)c.name.size());
  }
  const uint32_t cd_size = (uint32_t)o.tellp() - cd_off;
  w32(0x06054b50); w16(0); w16(0); w16((uint32_t)cd.size()); w16((uint32_t)cd.size()); w32(cd_size); w32(cd_off); w16(0);
  o.flush();
  if (!o) { *E = "Could not write " + S(zip) + " (disk full?)."; return false; }
  o.close();
  flush_path(tmp);
  if (!replace_file(tmp, zip)) { *E = "Could not finish " + S(zip); return false; }
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
  {
    std::string d = lower(dest);
    if (d.size() > 4 && d.compare(d.size() - 4, 4, ".zip") == 0) {
      if (!write_zip(gd, files, P(dest), &E)) return false;
      uint64_t bytes = 0;
      for (auto& f : files) bytes += f.second;
      R.dest = dest; R.generation = cur; R.bytes = bytes; R.files = (int)files.size();
      return true;
    }
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
        (lower(r.dest).size() > 4 && lower(r.dest).compare(lower(r.dest).size() - 4, 4, ".zip") == 0
             ? "\nRestore that zip with a save manager such as JKSV, or unzip it and copy the files into an emulator's save folder for this game."
             : "\nRestore that folder with a save manager such as JKSV or Checkpoint, or copy its files into the game's save folder of an emulator.");
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
    { std::string be; if (!savebackup::make(root, "import", nullptr, &be)) std::fprintf(stderr, "Backup before the import failed (%s); importing anyway, the import keeps the current save as a backup generation.\n", be.c_str()); }
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
