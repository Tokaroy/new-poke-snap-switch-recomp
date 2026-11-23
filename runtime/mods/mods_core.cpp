// SPDX-License-Identifier: MIT
#include "mods_core.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "../hle/json_lite.h"
#include "../hle/zip_read.h"

namespace fs = std::filesystem;

namespace mods {

uint32_t crc32(const void* data, size_t n) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  const uint8_t* p = (const uint8_t*)data;
  for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

static std::string lower(std::string s) {
  for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
  return s;
}

uint32_t ext_crc(const std::string& ext) {
  if (ext.size() == 8 && std::all_of(ext.begin(), ext.end(), [](unsigned char ch) { return std::isxdigit(ch); }))
    return (uint32_t)std::strtoul(ext.c_str(), nullptr, 16);
  std::string l = lower(ext);
  return crc32(l.data(), l.size());
}

std::string norm_archive(const std::string& a) {
  std::string s = a;
  for (auto& ch : s) if (ch == '\\') ch = '/';
  size_t colon = s.find(":/");
  if (colon != std::string::npos && colon < 12) s = s.substr(colon + 2);
  while (!s.empty() && s[0] == '/') s.erase(0, 1);
  return lower(s);
}

static bool ends_with_ci(const std::string& s, const std::string& suf) {
  return s.size() >= suf.size() && lower(s.substr(s.size() - suf.size())) == lower(suf);
}

bool parse_pack_path(const std::string& rel_in, std::string* archive, std::string* stem, uint32_t* extcrc) {
  std::string rel = rel_in;
  for (auto& ch : rel) if (ch == '\\') ch = '/';
  std::vector<std::string> parts;
  size_t i = 0;
  while (i <= rel.size()) {
    size_t j = rel.find('/', i);
    if (j == std::string::npos) j = rel.size();
    if (j > i) parts.push_back(rel.substr(i, j - i));
    i = j + 1;
  }
  size_t da = parts.size();
  for (size_t k = 0; k + 1 < parts.size(); k++)
    if (ends_with_ci(parts[k], ".drp")) { da = k; break; }
  if (da >= parts.size() || da + 2 != parts.size()) return false;
  const std::string& fn = parts.back();
  size_t dot = fn.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= fn.size()) return false;
  std::string ext = fn.substr(dot + 1);
  std::string st = fn.substr(0, dot);
  if (ends_with_ci(st, "." + ext)) st.resize(st.size() - ext.size() - 1);
  std::string arch;
  for (size_t k = 0; k <= da; k++) { if (k) arch += '/'; arch += parts[k]; }
  *archive = norm_archive(arch);
  *stem = lower(st);
  *extcrc = ext_crc(ext);
  return true;
}

std::string make_key(const std::string& archive_norm, const std::string& stem_lower, uint32_t extcrc) {
  char buf[16];
  snprintf(buf, sizeof buf, "|%08x|", extcrc);
  return archive_norm + buf + stem_lower;
}

void Index::add(int pack, const std::string& archive, const std::string& stem, uint32_t extcrc, const std::string& file, const std::string& rel) {
  std::string key = make_key(archive, stem, extcrc);
  auto it = map.find(key);
  if (it != map.end()) {   // a higher-priority pack was added first
    Conflict c;
    c.path = rel.empty() ? archive + "/" + stem : rel;
    c.winner = it->second.pack;
    c.loser = pack;
    c.winner_file = it->second.file;
    c.loser_file = file;
    conflicts.push_back(std::move(c));
    return;
  }
  Override o;
  o.file = file;
  o.pack = pack;
  o.label = archive + "/" + stem;
  o.rel = rel;
  map.emplace(std::move(key), std::move(o));
}

const Override* Index::find(const std::string& archive_norm, const std::string& name, uint32_t extcrc) const {
  std::string l = lower(name);
  auto it = map.find(make_key(archive_norm, l, extcrc));
  if (it != map.end()) return &it->second;
  size_t dot = l.rfind('.');
  if (dot != std::string::npos && ext_crc(l.substr(dot + 1)) == extcrc) {
    it = map.find(make_key(archive_norm, l.substr(0, dot), extcrc));
    if (it != map.end()) return &it->second;
  }
  return nullptr;
}

std::string default_root() {
  if (const char* e = std::getenv("SNAP_MODS_DIR")) if (*e) return e;
  const char* ap = std::getenv("APPDATA");
  return std::string(ap ? ap : ".") + "\\SnapRecomp\\mods";
}

std::string texdump_root() {
  if (const char* e = std::getenv("SNAP_TEXDUMP_DIR")) if (*e) return e;
  const char* la = std::getenv("LOCALAPPDATA");
  return std::string(la ? la : ".") + "\\SnapRecomp\\texcache";
}

bool texdump_enabled() {
  if (const char* e = std::getenv("SNAP_TEXDUMP")) return *e && std::strcmp(e, "0") != 0;
  std::error_code ec;
  return fs::exists(fs::path(texdump_root()) / "capture.on", ec);
}

std::string texdump_rel(const std::string& archive_norm, const std::string& name) {
  if (name.empty() || name.size() > 200 || archive_norm.empty() || archive_norm.find("..") != std::string::npos) return {};
  if (name == "." || name.find("..") != std::string::npos) return {};
  for (unsigned char ch : name)
    if (ch < 0x20 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') return {};
  for (unsigned char ch : archive_norm)
    if (ch < 0x20 || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') return {};
  if (archive_norm[0] == '/') return {};
  std::string n = name;
  if (n.find('.') == std::string::npos) n += ".nutexb";
  return archive_norm + "/" + n;
}

static bool read_text(const fs::path& p, std::string* out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

static void read_pack_json(Pack& p, std::vector<std::string>& warn) {
  std::string txt;
  if (!read_text(fs::path(p.dir) / "pack.json", &txt)) { warn.push_back(p.id + ": no pack.json (name and version unknown)"); p.name = p.id; return; }
  hle::Json j;
  std::string err;
  if (!hle::json_parse(txt, j, err)) { warn.push_back(p.id + ": pack.json: " + err); p.name = p.id; return; }
  if (auto* v = j.get("name")) p.name = v->str();
  if (auto* v = j.get("version")) p.version = v->str();
  if (auto* v = j.get("author")) p.author = v->str();
  if (auto* v = j.get("description")) p.description = v->str();
  if (p.name.empty()) p.name = p.id;
}

static fs::path U(const std::string& s) { return fs::u8path(s); }
static std::string S8(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }

// Size and time of the zip: its unpacked copy is current while this matches the copy's .stamp file.
static std::string zip_stamp(const fs::path& z) {
  std::error_code ec;
  uint64_t sz = (uint64_t)fs::file_size(z, ec);
  long long t = (long long)fs::last_write_time(z, ec).time_since_epoch().count();
  return std::to_string(sz) + " " + std::to_string(t) + " 1";
}

// A path from inside a zip may only name a file below the folder it is unpacked into.
static bool safe_rel(const std::string& r) {
  if (r.empty() || r[0] == '/' || r.find(':') != std::string::npos) return false;
  for (unsigned char c : r) if (c < 0x20) return false;
  size_t a = 0;
  while (a <= r.size()) {
    size_t b = r.find('/', a);
    if (b == std::string::npos) b = r.size();
    if (r.compare(a, b - a, "..") == 0 && b - a == 2) return false;
    a = b + 1;
  }
  return true;
}

// Where the pack starts inside the zip: "" when pack.json is at the top, else "<folder>/" when every file sits in one
// top folder (a zipped pack folder), else "".
static std::string zip_prefix(const std::vector<hle::zipr::Entry>& v) {
  std::string top;
  for (auto& e : v) {
    if (lower(e.rel) == "pack.json") return "";
    size_t k = e.rel.find('/');
    if (k == std::string::npos) return "";
    std::string t = e.rel.substr(0, k + 1);
    if (top.empty()) top = t;
    else if (lower(t) != lower(top)) return "";
  }
  return top;
}

static fs::path unpacked_base(const std::string& root) { return U(root) / ".unpacked"; }

std::vector<std::string> packs_on_disk(const std::string& root) {
  std::vector<std::string> ids;
  std::error_code ec;
  for (fs::directory_iterator it(U(root), ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    std::string name = S8(it->path().filename());
    if (name.empty() || name[0] == '.') continue;
    if (it->is_directory(e2)) {
      if (fs::exists(it->path() / "pack.json", e2)) ids.push_back(name);
    } else if (it->is_regular_file(e2) && lower(name).size() > 4 && lower(name).compare(name.size() - 4, 4, ".zip") == 0) {
      ids.push_back(name.substr(0, name.size() - 4));
    }
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());   // a folder and a zip of the same name: one pack (the folder is used)
  return ids;
}

// A pack id names one folder or zip directly under the mods root: no separators, drive colons, or "." / "..". Windows strips
// trailing dots and spaces from a path name, so ".. " or ". " would name the mods root (or its parent) and unpack_zip's
// remove_all would wipe it: an id may not end in a dot or a space. DOS device names (con, nul, com1, ...) are refused too,
// with or without an extension, and the id must stay a single name directly under a base folder after normalizing.
bool safe_pack_id(const std::string& id) {
  if (id.empty() || id == "." || id == "..") return false;
  if (id.back() == '.' || id.back() == ' ') return false;
  for (char c : id) {
    const unsigned char u = (unsigned char)c;
    if (u < 0x20) return false;                                     // NUL and control characters
    if (std::strchr("/\\:*?\"<>|", c)) return false;               // separators, drive colons, wildcards, other Windows-invalid
  }
  // the device check looks at the name before the first dot, without trailing spaces ("nul .zip" is a device too)
  std::string stem = lower(id.substr(0, id.find('.')));
  while (!stem.empty() && stem.back() == ' ') stem.pop_back();
  static const char* const kDev[] = {"con", "prn", "aux", "nul", "conin$", "conout$"};
  for (const char* d : kDev) if (stem == d) return false;
  if (stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) && stem[3] >= '0' && stem[3] <= '9') return false;
  if (stem.size() == 5 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) && (unsigned char)stem[3] == 0xC2 &&
      ((unsigned char)stem[4] == 0xB9 || (unsigned char)stem[4] == 0xB2 || (unsigned char)stem[4] == 0xB3)) return false;   // COM¹²³
  const fs::path base = U("mods_root"), p = (base / U(id)).lexically_normal();
  if (p.parent_path() != base || p.filename() != U(id)) return false;
  return true;
}

std::string unpack_zip(const std::string& root, const std::string& id, std::vector<std::string>& warn) {
  std::error_code ec;
  if (!safe_pack_id(id)) { warn.push_back("pack id \"" + id + "\" refused (not a plain folder name)"); return ""; }
  fs::path z = U(root) / U(id + ".zip"), base = unpacked_base(root), dst = base / U(id), st = base / U(id + ".stamp");
  const std::string want = zip_stamp(z);
  std::string have;
  if (read_text(st, &have) && have == want && fs::is_directory(dst, ec)) return S8(dst);
  std::vector<hle::zipr::Entry> v;
  std::string err;
  if (!hle::zipr::list(z, v, &err)) { warn.push_back(id + ".zip: " + err); return ""; }
  const std::string pre = zip_prefix(v);
  fs::path tmp = base / U(id + ".part");
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);
  for (auto& e : v) {
    if (lower(e.rel.substr(0, pre.size())) != lower(pre)) continue;
    std::string r = e.rel.substr(pre.size());
    if (!safe_rel(r)) { warn.push_back(id + ".zip: refused " + e.rel + " (it would land outside the pack)"); fs::remove_all(tmp, ec); return ""; }
    fs::path out = tmp / U(r);
    fs::create_directories(out.parent_path(), ec);
    if (!hle::zipr::extract(z, e, out, &err)) { warn.push_back(id + ".zip: " + err); fs::remove_all(tmp, ec); return ""; }
  }
  fs::remove(st, ec);
  fs::remove_all(dst, ec);
  ec.clear();
  fs::rename(tmp, dst, ec);
  if (ec) { warn.push_back(id + ".zip: could not move the unpacked copy into place: " + ec.message()); fs::remove_all(tmp, ec); return ""; }
  std::ofstream(st, std::ios::binary | std::ios::trunc) << want;
  return S8(dst);
}

bool pack_info(const std::string& root, const std::string& id, Pack& p, std::vector<std::string>& warn) {
  std::error_code ec;
  p = Pack();
  p.id = id;
  if (!safe_pack_id(id)) { warn.push_back("pack id \"" + id + "\" refused (not a plain folder name)"); return false; }
  fs::path folder = U(root) / U(id), z = U(root) / U(id + ".zip");
  if (fs::is_directory(folder, ec)) {
    p.dir = S8(folder);
    read_pack_json(p, warn);
    return true;
  }
  if (!fs::is_regular_file(z, ec)) return false;
  p.zip = true;
  p.dir = S8(unpacked_base(root) / U(id));
  p.name = id;
  std::vector<hle::zipr::Entry> v;
  std::string err;
  if (!hle::zipr::list(z, v, &err)) { warn.push_back(id + ".zip: " + err); return true; }
  const std::string want = lower(zip_prefix(v) + "pack.json");
  for (auto& e : v) {
    if (lower(e.rel) != want || e.size > (1u << 20)) continue;
    Pack q;
    q.id = id;
    q.dir = S8(unpacked_base(root) / U(id + ".meta"));
    fs::create_directories(U(q.dir), ec);
    if (hle::zipr::extract(z, e, U(q.dir) / "pack.json", &err)) {
      read_pack_json(q, warn);
      p.name = q.name; p.version = q.version; p.author = q.author; p.description = q.description;
    } else {
      warn.push_back(id + ".zip: " + err);
    }
    fs::remove_all(U(q.dir), ec);
    break;
  }
  return true;
}

// mods.json as written: (id, enabled) in order. A plain string is enabled; "folder" is an old spelling of "id".
static std::vector<std::pair<std::string, bool>> read_order_raw(const std::string& root, std::vector<std::string>& warn) {
  std::vector<std::pair<std::string, bool>> order;
  std::string txt;
  if (!read_text(fs::path(root) / "mods.json", &txt)) return order;
  hle::Json j;
  std::string err;
  if (!hle::json_parse(txt, j, err)) { warn.push_back("mods.json: " + err); return order; }
  if (auto* arr = j.get("packs")) {
    for (auto& e : arr->a) {
      if (e.type == hle::Json::Str) order.push_back({e.s, true});
      else if (e.type == hle::Json::Obj) {
        std::string id = e.get("id") ? e.get("id")->str() : e.get("folder") ? e.get("folder")->str() : "";
        if (!id.empty()) order.push_back({id, e.get("enabled") ? e.get("enabled")->boolean(true) : true});
      }
    }
  }
  return order;
}

void load(const std::string& root, Index& out, std::vector<std::string>& warn, bool unpack) {
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return;
  std::vector<std::pair<std::string, bool>> order = read_order_raw(root, warn);   // (folder, enabled)
  // Without a mods.json nothing is enabled (the owner parked texture and mod packs; before 2026-11-13 every folder with a pack.json was).
  // Packs the game itself needs are the exception and are on unless mods.json lists them as disabled: "native-options" is the PC tabs of the
  // game's Settings screen that host/play.ps1 builds (host/mods/nativeopt.cpp reads its rows.txt only while that pack is active).
  for (const char* req : {"native-options"}) {
    bool listed = false;
    for (auto& o : order) if (o.first == req) listed = true;
    if (!listed && fs::exists(fs::path(root) / req / "pack.json", ec)) order.push_back({req, true});
  }
  std::unordered_map<std::string, int> rom_seen;   // romfs/ path -> the pack that serves it
  for (auto& [id, en] : order) {
    Pack p;
    if (!safe_pack_id(id)) { warn.push_back("mods.json: pack id \"" + id + "\" skipped (not a plain folder name)"); continue; }
    if (!pack_info(root, id, p, warn)) { warn.push_back(id + ": listed in mods.json but there is no folder or .zip of that name"); continue; }
    p.enabled = en;
    bool listed_only = false;   // an enabled zip read from its file list (unpack = false)
    if (p.zip && en) {
      if (unpack) {
        std::string d = unpack_zip(root, id, warn);
        if (d.empty()) { p.enabled = false; out.packs.push_back(p); continue; }
        p.dir = d;
        read_pack_json(p, warn);
      } else {
        listed_only = true;
      }
    }
    out.packs.push_back(p);
    if (!p.enabled) continue;
    int idx = (int)out.packs.size() - 1;
    // The pack's files as (path below the pack, file on disk; empty for a zip that is only listed).
    std::vector<std::pair<std::string, std::string>> files;
    if (listed_only) {
      std::vector<hle::zipr::Entry> v;
      std::string err;
      if (!hle::zipr::list(U(root) / U(id + ".zip"), v, &err)) { warn.push_back(id + ".zip: " + err); continue; }
      const std::string pre = zip_prefix(v);
      bool bad = false;
      for (auto& e : v) {
        if (lower(e.rel.substr(0, pre.size())) != lower(pre)) continue;
        std::string r = e.rel.substr(pre.size());
        if (!safe_rel(r)) { warn.push_back(id + ".zip: refused " + e.rel + " (it would land outside the pack)"); bad = true; break; }
        files.push_back({r, ""});
      }
      if (bad) { out.packs[(size_t)idx].enabled = false; continue; }   // unpack_zip refuses it the same way
    } else {
      // <pack>/romfs/<path>: whole files laid over rom:/ (MountRom puts these folders first). Used for archives with added
      // entries (scripts/hd/drp_add_entries.py); loose entry files below still override entries inside them.
      if (fs::is_directory(fs::path(p.dir) / "romfs", ec)) out.rom_layers.push_back({(fs::path(p.dir) / "romfs").string(), idx});
      for (auto it = fs::recursive_directory_iterator(p.dir, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        files.push_back({fs::relative(it->path(), p.dir, ec).generic_string(), it->path().string()});
      }
    }
    for (auto& [rel, file] : files) {
      if (lower(rel).compare(0, 6, "romfs/") == 0) {   // whole-file layer, not entries
        auto r = rom_seen.emplace(lower(rel), idx);     // the first layer that has a path wins, the same rule MountRom uses
        if (!r.second) out.conflicts.push_back(Conflict{rel, r.first->second, idx});
        continue;
      }
      if (rel == "pack.json" || lower(rel).find(".drp/") == std::string::npos) continue;   // only paths below an archive folder
      std::string a, s;
      uint32_t x;
      if (!parse_pack_path(rel, &a, &s, &x)) { warn.push_back(p.id + ": ignored " + rel + " (expected <archive>.drp/<entry>.<ext>)"); continue; }
      out.add(idx, a, s, x, file, rel);
    }
  }
}

// ---- Order and conflicts for the command line and the launcher.
std::vector<OrderEntry> list_order(const std::string& root, std::vector<std::string>& warn) {
  std::vector<OrderEntry> v;
  auto have = [&](const std::string& id) { for (auto& e : v) if (lower(e.id) == lower(id)) return true; return false; };
  auto add = [&](const std::string& id, bool on, bool listed) {
    Pack p;
    if (id == "native-options" || have(id) || !pack_info(root, id, p, warn)) return;
    OrderEntry e;
    e.id = id; e.enabled = on; e.listed = listed; e.name = p.name.empty() ? id : p.name;
    v.push_back(std::move(e));
  };
  for (auto& o : read_order_raw(root, warn)) add(o.first, o.second, true);
  for (const std::string& id : packs_on_disk(root)) add(id, false, false);
  return v;
}

bool save_order(const std::string& root, const std::vector<OrderEntry>& visible, std::string* err) {
  std::vector<std::string> warn;
  std::vector<std::pair<std::string, bool>> raw = read_order_raw(root, warn);
  auto shown = [&](const std::string& id) { for (auto& e : visible) if (lower(e.id) == lower(id)) return true; return false; };
  hle::Json arr = hle::Json::array();
  auto put = [&](const std::string& id, bool on) {
    hle::Json o = hle::Json::object();
    o.set("id", hle::Json::string(id));
    o.set("enabled", hle::Json::boolean_(on));
    arr.a.push_back(std::move(o));
  };
  for (auto& e : visible) put(e.id, e.enabled);
  std::vector<std::string> done;
  for (auto& r : raw) {   // native-options and packs whose folder is missing stay as they were
    bool dup = false;
    for (auto& d : done) if (lower(d) == lower(r.first)) dup = true;
    if (shown(r.first) || dup) continue;
    done.push_back(r.first);
    put(r.first, r.second);
  }
  hle::Json doc = hle::Json::object();
  std::string txt;
  hle::Json old;
  std::string perr;
  if (read_text(fs::path(root) / "mods.json", &txt) && hle::json_parse(txt, old, perr) && old.type == hle::Json::Obj) doc = old;
  doc.set("packs", arr);
  std::string out;
  hle::json_dump(doc, out);
  std::error_code ec;
  fs::create_directories(fs::path(root), ec);
  fs::path f = fs::path(root) / "mods.json", tmp = fs::path(root) / "mods.json.tmp";
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    o << out << "\n";
    if (!o) { if (err) *err = "could not write " + tmp.string(); return false; }
  }
  fs::rename(tmp, f, ec);
  if (ec) { ec.clear(); fs::remove(f, ec); ec.clear(); fs::rename(tmp, f, ec); }
  if (ec) { if (err) *err = "could not replace mods.json: " + ec.message(); return false; }
  return true;
}

std::vector<PairCount> conflict_pairs(const Index& ix) {
  std::vector<PairCount> v;
  for (const Conflict& c : ix.conflicts) {
    const std::string& w = ix.packs[(size_t)c.winner].id;
    const std::string& l = ix.packs[(size_t)c.loser].id;
    bool found = false;
    for (auto& p : v) if (p.winner == w && p.loser == l) { p.count++; found = true; break; }
    if (!found) v.push_back({w, l, 1});
  }
  std::stable_sort(v.begin(), v.end(), [](const PairCount& a, const PairCount& b) { return a.count > b.count; });
  return v;
}

std::string cli(const std::string& root, const std::vector<std::string>& args, int* code) {
  int rc = 0;
  if (!code) code = &rc;
  *code = 0;
  std::string out;
  const std::string usage =
      "usage: --mods list\n"
      "       --mods conflicts [--files] [<pack id>]\n"
      "       --mods order <id> [<id> ...]      put these packs on top, in this order\n"
      "       --mods move <id> up|down|top|bottom|<position>\n"
      "       --mods enable|disable <id> [<id> ...]\n"
      "The first pack wins when two change the same file. Changes apply at the next game start.\n";
  const std::string verb = args.empty() ? "list" : lower(args[0]);
  std::vector<std::string> warn;
  std::vector<OrderEntry> v = list_order(root, warn);
  auto find = [&](const std::string& id) -> int { for (size_t i = 0; i < v.size(); i++) if (lower(v[i].id) == lower(id)) return (int)i; return -1; };
  auto warnings = [&]() { for (auto& w : warn) out += "warning: " + w + "\n"; };
  auto fail = [&](const std::string& m, int c) { out += m + "\n"; *code = c; return out; };
  if (verb == "list") {
    out += "mods folder: " + root + "\n";
    if (v.empty()) out += "no packs\n";
    for (size_t i = 0; i < v.size(); i++) {
      char line[64];
      snprintf(line, sizeof line, "%2zu. [%s] ", i + 1, v[i].enabled ? "on " : "off");
      out += line + v[i].id + (v[i].name != v[i].id ? "  (" + v[i].name + ")" : "") + (v[i].listed ? "" : "  (not in mods.json)") + "\n";
    }
    warnings();
    return out;
  }
  if (verb == "conflicts") {
    bool files = false;
    std::string only;
    for (size_t i = 1; i < args.size(); i++) { if (args[i] == "--files") files = true; else only = args[i]; }
    if (!only.empty() && find(only) < 0) return fail("no such pack: " + only, 2);
    Index ix;
    load(root, ix, warn, false);
    int shown = 0;
    if (files) {
      for (const Conflict& c : ix.conflicts) {
        const std::string& w = ix.packs[(size_t)c.winner].id;
        const std::string& l = ix.packs[(size_t)c.loser].id;
        if (!only.empty() && lower(only) != lower(w) && lower(only) != lower(l)) continue;
        out += c.path + ": " + w + " beats " + l + "\n";
        shown++;
      }
    } else {
      for (const PairCount& p : conflict_pairs(ix)) {
        if (!only.empty() && lower(only) != lower(p.winner) && lower(only) != lower(p.loser)) continue;
        out += std::to_string(p.count) + " file" + (p.count == 1 ? "" : "s") + ": " + p.winner + " beats " + p.loser + "\n";
        shown++;
      }
    }
    if (!shown) out += "no conflicts between enabled packs\n";
    else if (!files) out += "add --files to list every file\n";
    warnings();
    return out;
  }
  if (verb == "enable" || verb == "disable") {
    if (args.size() < 2) return fail(usage, 2);
    for (size_t i = 1; i < args.size(); i++) {
      int k = find(args[i]);
      if (k < 0) return fail("no such pack: " + args[i], 2);
      v[(size_t)k].enabled = verb == "enable";
    }
  } else if (verb == "order") {
    if (args.size() < 2) return fail(usage, 2);
    std::vector<OrderEntry> top;
    for (size_t i = 1; i < args.size(); i++) {
      int k = find(args[i]);
      if (k < 0) return fail("no such pack: " + args[i], 2);
      bool dup = false;
      for (auto& t : top) if (lower(t.id) == lower(args[i])) dup = true;
      if (dup) return fail("pack named twice: " + args[i], 2);
      top.push_back(v[(size_t)k]);
    }
    for (auto& e : v) { bool in = false; for (auto& t : top) if (lower(t.id) == lower(e.id)) in = true; if (!in) top.push_back(e); }
    v = top;
  } else if (verb == "move") {
    if (args.size() != 3) return fail(usage, 2);
    int k = find(args[1]);
    if (k < 0) return fail("no such pack: " + args[1], 2);
    const std::string w = lower(args[2]);
    int to = k;
    if (w == "up") to = k - 1;
    else if (w == "down") to = k + 1;
    else if (w == "top") to = 0;
    else if (w == "bottom") to = (int)v.size() - 1;
    else if (!w.empty() && w.size() < 5 && w.find_first_not_of("0123456789") == std::string::npos) to = atoi(w.c_str()) - 1;
    else return fail(usage, 2);
    to = std::clamp(to, 0, (int)v.size() - 1);
    OrderEntry e = v[(size_t)k];
    v.erase(v.begin() + k);
    v.insert(v.begin() + to, e);
  } else if (verb == "--help" || verb == "help" || verb == "-h") {
    return usage;
  } else {
    return fail("unknown --mods command: " + args[0] + "\n" + usage, 2);
  }
  std::string err;
  if (!save_order(root, v, &err)) return fail("mods.json was not changed: " + err, 1);
  out += "mods.json saved. Order now:\n";
  for (size_t i = 0; i < v.size(); i++) out += std::to_string(i + 1) + ". [" + (v[i].enabled ? "on" : "off") + "] " + v[i].id + "\n";
  return out;
}

size_t chunk_header_len(size_t name_len) { return 0x18 + ((name_len + 4) & ~(size_t)3); }

std::vector<uint8_t> build_chunk(const uint8_t* orig, size_t hdr_len, const uint8_t* data, size_t n) {
  std::vector<uint8_t> v(hdr_len + n);
  std::memcpy(v.data(), orig, hdr_len);
  if (n) std::memcpy(v.data() + hdr_len, data, n);
  uint32_t crc = crc32(data, n), total = (uint32_t)(hdr_len + n), sz = (uint32_t)n;
  std::memcpy(v.data() + 0x00, &crc, 4);
  std::memcpy(v.data() + 0x08, &total, 4);
  std::memcpy(v.data() + 0x10, &sz, 4);
  std::memcpy(v.data() + 0x14, &sz, 4);
  return v;
}

}  // namespace mods
