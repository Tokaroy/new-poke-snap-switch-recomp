// SPDX-License-Identifier: GPL-3.0-or-later
// An exe in a folder the player cannot write to (Program Files, a read-only share, Controlled Folder Access): the pure parts. Header only and
// free of Windows includes, so tests/hle/hle_tests.cpp checks it on its own. The dialogs, the copy and the restart are host/readonly_install.cpp;
// the one place that turns a data folder into APPDATA / LOCALAPPDATA is still snap_portable_init (host/portable.cpp, --data-dir).
// docs/readonly-install.md.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace snap_ro {

namespace fs = std::filesystem;

// UTF-8 text of a path; u8string() is std::u8string in C++20 and std::string in C++17, and this builds in both.
inline std::string to_u8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

inline std::string lower_ascii(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return s;
}

// Tries what the game does there: make a folder, make a file, write bytes, flush, delete both. Nothing is judged from the path or from the
// folder's permissions, because Controlled Folder Access and a read-only share refuse the write while the ACLs look fine.
// A probe file that cannot be deleted again counts as a failure (the folder is not really ours). Returns true when all of it worked.
inline bool probe_writable(const fs::path& dir, std::string* why = nullptr) {
  static std::atomic<unsigned> counter{0};
  std::error_code ec;
  auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
  if (!fs::is_directory(dir, ec)) return fail("the folder does not exist");
  const auto tick = (unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count();
  const std::string stem = ".snap_write_probe_" + std::to_string(tick) + "_" + std::to_string(counter++);
  const fs::path sub = dir / (stem + ".d"), file = sub / "probe.tmp";
  if (!fs::create_directory(sub, ec) || ec) return fail("a folder could not be created (" + (ec ? ec.message() : std::string("refused")) + ")");
  bool wrote = false;
  {
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (f) {
      f << "snap write probe";
      f.flush();
      wrote = f.good();
    }
  }
  std::string err;
  if (!wrote) err = "a file could not be written";
  fs::remove(file, ec);
  if (ec && err.empty()) err = "a file could not be deleted again (" + ec.message() + ")";
  ec.clear();
  fs::remove(sub, ec);
  if (ec && err.empty()) err = "a folder could not be deleted again (" + ec.message() + ")";
  if (!err.empty()) return fail(err);
  return true;
}

// ---- the pointer file: one line per exe, "<exe path, lower case>\t<data folder>". It lives in a folder the player can always write (the real
// %LOCALAPPDATA%\SnapRecomp), because the read-only exe folder cannot hold it and the data folder cannot be found without it.

inline std::string pointer_key(const std::string& exe_path) {
  std::string k = lower_ascii(exe_path);
  for (char& c : k) if (c == '/') c = '\\';
  while (k.size() > 3 && k.back() == '\\') k.pop_back();
  return k;
}

inline std::string pointer_lookup(const std::string& text, const std::string& exe_path) {
  const std::string key = pointer_key(exe_path);
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t tab = line.find('\t');
    if (tab == std::string::npos || tab == 0) continue;
    if (line.compare(0, tab, key) == 0 && tab == key.size()) return line.substr(tab + 1);
  }
  return "";
}

// The text with this exe's line replaced (or added). An empty data_dir removes the line.
inline std::string pointer_update(const std::string& text, const std::string& exe_path, const std::string& data_dir) {
  const std::string key = pointer_key(exe_path);
  std::string out;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t tab = line.find('\t');
    if (line.empty() || tab == std::string::npos || tab == 0) continue;
    if (tab == key.size() && line.compare(0, tab, key) == 0) continue;
    out += line + "\n";
  }
  if (!data_dir.empty() && data_dir.find_first_of("\t\r\n") == std::string::npos) out += key + "\t" + data_dir + "\n";
  return out;
}

// ---- what to do at start

enum class Action {
  Normal,        // nothing to do: start as usual
  UseDataDir,    // redirect all writable data to Plan::data_dir (the folder remembered for this exe), no question
  Ask,           // show the choice (a window may open)
  Fail,          // no window may open: print a message and exit nonzero
};

struct Inputs {
  bool data_dir_flag = false;         // --data-dir was given: the caller chose, snap_portable_init reports a folder that cannot be written
  bool redirected_by_parent = false;  // SNAP_PORTABLE=1 is already set: a child of a run that settled this
  bool managed_layout = false;        // config.json or portable.txt sits next to the exe: a release build that keeps its data beside itself
  bool exe_dir_writable = true;       // probe_writable of the exe's folder
  bool interactive = false;           // a window may open (no --play, no action, no --no-prompt, no test run)
  std::string pointer_dir;            // the folder remembered for this exe, or ""
  bool pointer_dir_writable = false;  // probe_writable of that folder
};

struct Plan {
  Action action = Action::Normal;
  std::string data_dir;
};

inline Plan resolve(const Inputs& in) {
  Plan p;
  if (in.data_dir_flag || in.redirected_by_parent || !in.managed_layout || in.exe_dir_writable) return p;
  if (!in.pointer_dir.empty() && in.pointer_dir_writable) { p.action = Action::UseDataDir; p.data_dir = in.pointer_dir; return p; }
  p.action = in.interactive ? Action::Ask : Action::Fail;
  return p;
}

// ---- moving the install

// A lower-case, slash-separated, trailing-slash form of a path for comparing two paths.
inline std::string norm_key(const fs::path& p) {
  const auto s = (p.lexically_normal() / "").generic_u8string();
  return lower_ascii(std::string(s.begin(), s.end()));
}

// True when a is b or inside b (lower-cased lexical compare).
inline bool inside_or_same(const fs::path& a, const fs::path& b) {
  const std::string x = norm_key(a), y = norm_key(b);
  return x.size() >= y.size() && x.compare(0, y.size(), y) == 0;
}

// The folder that is moved as a whole: <root> when the exe sits in <root>\<exe_rel> (config.json "install_root" and "exe_rel", host/portable.h:
// the next start then rewrites every stored path under the old root), else the exe's own folder.
inline fs::path move_source(const fs::path& exe_dir, const std::string& install_root, const std::string& exe_rel) {
  if (install_root.empty() || exe_rel.empty()) return exe_dir;
  const fs::path expect = (fs::u8path(install_root) / fs::u8path(exe_rel)).lexically_normal();
  if (norm_key(expect) != norm_key(exe_dir)) return exe_dir;
  return fs::u8path(install_root).lexically_normal();
}

struct CopyStats {
  std::atomic<unsigned long long> files{0}, bytes{0};
};

// Copies the whole tree src into dst (dst is created). Returns false and fills why on the first failure; nothing already copied is removed.
inline bool copy_tree(const fs::path& src, const fs::path& dst, CopyStats* st, std::string* why) {
  std::error_code ec;
  fs::create_directories(dst, ec);
  if (ec) { if (why) *why = "could not create " + to_u8(dst) + ": " + ec.message(); return false; }
  for (fs::recursive_directory_iterator it(src, fs::directory_options::none, ec), end; it != end; it.increment(ec)) {
    if (ec) break;
    std::error_code e2;
    const fs::path rel = fs::relative(it->path(), src, e2);
    const fs::path to = dst / rel;
    if (it->is_directory(e2)) {
      fs::create_directories(to, e2);
      if (e2) { if (why) *why = "could not create " + to_u8(to) + ": " + e2.message(); return false; }
    } else if (it->is_regular_file(e2)) {
      fs::copy_file(it->path(), to, fs::copy_options::overwrite_existing, e2);
      if (e2) { if (why) *why = "could not copy " + to_u8(rel) + ": " + e2.message(); return false; }
      if (st) { st->files++; st->bytes += (unsigned long long)it->file_size(e2); }
    }
  }
  if (ec) { if (why) *why = "could not read " + to_u8(src) + ": " + ec.message(); return false; }
  return true;
}

// Every file and folder of src exists in dst with the same size, and the file full_compare_rel (when not empty) is identical byte for byte.
inline bool verify_tree(const fs::path& src, const fs::path& dst, const fs::path& full_compare_rel, std::string* why) {
  std::error_code ec;
  for (fs::recursive_directory_iterator it(src, fs::directory_options::none, ec), end; it != end; it.increment(ec)) {
    if (ec) break;
    std::error_code e2;
    const fs::path rel = fs::relative(it->path(), src, e2);
    if (it->is_directory(e2)) {
      if (!fs::is_directory(dst / rel, e2)) { if (why) *why = "the copy lacks the folder " + to_u8(rel); return false; }
    } else if (it->is_regular_file(e2)) {
      const auto a = it->file_size(e2);
      const auto b = fs::file_size(dst / rel, e2);
      if (e2 || a != b) { if (why) *why = "the copy of " + to_u8(rel) + " differs in size"; return false; }
    }
  }
  if (ec) { if (why) *why = "could not read " + to_u8(src) + ": " + ec.message(); return false; }
  if (!full_compare_rel.empty()) {
    std::ifstream a(src / full_compare_rel, std::ios::binary), b(dst / full_compare_rel, std::ios::binary);
    if (!a || !b) { if (why) *why = "the copy of " + to_u8(full_compare_rel) + " cannot be read"; return false; }
    std::vector<char> x(65536), y(65536);
    while (a) {
      a.read(x.data(), (std::streamsize)x.size());
      const std::streamsize n = a.gcount();
      b.read(y.data(), n);
      if (b.gcount() != n || std::memcmp(x.data(), y.data(), (size_t)n) != 0) { if (why) *why = "the copy of " + to_u8(full_compare_rel) + " differs"; return false; }
    }
  }
  return true;
}

}  // namespace snap_ro
