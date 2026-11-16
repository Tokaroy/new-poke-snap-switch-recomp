// SPDX-License-Identifier: MIT
// Install check: see install_check.h. Python twin: builder/snapbuild/installcheck.py (same wording where it can be).
#include "install_check.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include "../../runtime/hle/json_lite.h"

namespace fs = std::filesystem;

namespace snap_install {
namespace {

std::string U8(const std::wstring& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}
std::wstring W(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
std::string lower(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
  return s;
}

// Streaming SHA-256 (BCrypt).
class Sha {
 public:
  Sha() {
    if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) { alg_ = nullptr; return; }
    if (BCryptCreateHash(alg_, &h_, nullptr, 0, nullptr, 0, 0) != 0) h_ = nullptr;
  }
  ~Sha() { if (h_) BCryptDestroyHash(h_); if (alg_) BCryptCloseAlgorithmProvider(alg_, 0); }
  void add(const void* p, size_t n) { if (h_) BCryptHashData(h_, (PUCHAR)p, (ULONG)n, 0); }
  std::string hex() {
    unsigned char d[32] = {};
    if (!h_ || BCryptFinishHash(h_, d, 32, 0) != 0) return "";
    static const char* hx = "0123456789abcdef";
    std::string o;
    for (unsigned char c : d) { o += hx[c >> 4]; o += hx[c & 15]; }
    return o;
  }
 private:
  BCRYPT_ALG_HANDLE alg_ = nullptr;
  BCRYPT_HASH_HANDLE h_ = nullptr;
};

std::string sha_of(const std::string& s) { Sha h; h.add(s.data(), s.size()); return h.hex(); }

// Size and SHA-256 of a file; false when it cannot be read.
bool file_sha(const fs::path& p, long long* size, std::string* hex) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  Sha h;
  std::vector<char> buf(1 << 20);
  long long total = 0;
  while (f) {
    f.read(buf.data(), (std::streamsize)buf.size());
    const std::streamsize g = f.gcount();
    if (g <= 0) break;
    h.add(buf.data(), (size_t)g);
    total += g;
  }
  *size = total;
  *hex = h.hex();
  return true;
}

bool read_json(const fs::path& p, hle::Json& j) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  std::string err;
  return hle::json_parse(ss.str(), j, err);
}

struct Out {
  Report r;
  int problems = 0;
  void ok(const std::string& s) { r.lines.push_back("ok:      " + s); }
  void bad(const std::string& s) { ++problems; r.lines.push_back("PROBLEM: " + s); }
};

}  // namespace

Report run(const fs::path& exe_dir) {
  Out o;
  hle::Json cfg, data, rec;
  const bool have_cfg = read_json(exe_dir / L"config.json", cfg) && cfg.type == hle::Json::Obj;
  const bool have_data = read_json(exe_dir / L"launcher_data.json", data) && data.type == hle::Json::Obj;
  const bool have_rec = read_json(exe_dir / L"install_record.json", rec) && rec.type == hle::Json::Obj;
  std::error_code ec;

  // ---- RomFS: the 2.0.1 check files (hashed path, exact size) and the hash over the whole file list
  const hle::Json* sents = have_data ? data.get("sentinels") : nullptr;
  const std::string root = have_cfg && cfg.get("romfs_root") ? cfg.get("romfs_root")->str() : "";
  if (!sents || sents->a.empty()) {
    o.r.lines.push_back("note:    no published RomFS check ships with this build, so the RomFS was not compared.");
  } else if (root.empty() || !fs::is_directory(fs::u8path(root), ec)) {
    o.bad("RomFS (game data folder) not found. Is the drive or network share connected? Use \"Change game folder\" to pick it again.");
  } else {
    std::vector<std::pair<std::string, long long>> listing;
    const fs::path base = fs::u8path(root);
    for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e2;
      if (!it->is_regular_file(e2)) continue;
      listing.emplace_back(lower(U8(fs::relative(it->path(), base, e2).generic_wstring())), (long long)it->file_size(e2));
    }
    std::sort(listing.begin(), listing.end());
    std::map<std::string, long long> by_key;
    for (auto& e : listing) by_key[sha_of(e.first)] = e.second;
    size_t missing = 0, wrong = 0;
    for (auto& s : sents->a) {
      const hle::Json* k = s.get("path_sha256");
      const hle::Json* z = s.get("size");
      if (!k || !z) continue;
      auto f = by_key.find(k->str());
      if (f == by_key.end()) ++missing;
      else if (f->second != (long long)z->num()) ++wrong;
    }
    const size_t n = sents->a.size();
    const hle::Json* inst = data.get("install");
    const hle::Json* want_list = inst ? inst->get("romfs_listing_sha256") : nullptr;
    const hle::Json* want_cnt = data.get("file_count");
    std::string expected_files = want_cnt ? std::to_string((long long)want_cnt->num()) : "?";
    if (listing.empty()) {
      o.bad("RomFS (game data folder) has no files. Extract the RomFS there first.");
    } else if (missing == n) {
      o.bad("the game data folder does not look like this game's RomFS: " + std::to_string(listing.size()) + " files found and none of the " +
            std::to_string(n) + " check files are in it. Use \"Change game folder\" to pick the folder that holds the RomFS tree.");
    } else if (missing || wrong) {
      o.bad("RomFS (game data folder) is not the 2.0.1 RomFS: " + std::to_string(missing) + " of " + std::to_string(n) + " check files are missing and " +
            std::to_string(wrong) + " have the wrong size. It looks like the base game or an update before 2.0.1. Apply the 2.0.1 update and extract it again.");
    } else {
      bool list_ok = true;
      if (want_list && !want_list->str().empty()) {
        Sha h;
        for (auto& e : listing) { std::string l = e.first + "\t" + std::to_string(e.second) + "\n"; h.add(l.data(), l.size()); }
        list_ok = h.hex() == want_list->str();
      }
      if (!list_ok) o.bad("RomFS (game data folder) has the 2.0.1 check files but its file list differs (" + std::to_string(listing.size()) + " files found, " +
                          expected_files + " expected): files were added, removed or resized. Extract the patched RomFS into an empty folder.");
      else o.ok("RomFS (game data folder) is game version 2.0.1 (" + std::to_string(listing.size()) + " files).");
    }
  }

  // ---- the built program folder against install_record.json, and the shared DLLs against the known-good list
  int checked = 0, bad_files = 0;
  if (!have_rec) {
    o.r.lines.push_back("note:    install_record.json is not next to the program (the build is older than this check), so the exe and the converted game images were not compared. Run the builder or the setup again.");
  } else {
    const hle::Json* files = rec.get("files");
    if (files && files->type == hle::Json::Obj) {
      for (auto& kv : files->o) {
        ++checked;
        long long sz = 0; std::string hx;
        const hle::Json* want_sz = kv.second.get("size");
        const hle::Json* want_h = kv.second.get("sha256");
        if (!file_sha(exe_dir / fs::u8path(kv.first), &sz, &hx)) { ++bad_files; o.bad(kv.first + " is missing. Run the builder or the setup again to restore it."); }
        else if (!want_sz || !want_h || sz != (long long)want_sz->num() || hx != want_h->str()) {
          ++bad_files; o.bad(kv.first + " was changed after the build (size or contents differ from the build record). Run the builder or the setup again to restore it.");
        }
      }
    }
    const hle::Json* ext = rec.get("external");
    if (ext && ext->type == hle::Json::Obj) {
      for (auto& kv : ext->o) {
        ++checked;
        std::string label = kv.first == "main_elf" ? "converted main game image" : kv.first == "sdk_elf" ? "converted sdk game image" :
                            kv.first == "main_nso" ? "main program file used for the build" : kv.first == "sdk_nso" ? "sdk program file used for the build" : kv.first;
        const hle::Json* pth = kv.second.get("path");
        const hle::Json* want_sz = kv.second.get("size");
        const hle::Json* want_h = kv.second.get("sha256");
        long long sz = 0; std::string hx;
        if (!pth || !file_sha(fs::u8path(pth->str()), &sz, &hx)) { ++bad_files; o.bad("The " + label + " is missing. " + (kv.first.find("nso") != std::string::npos ? "Pick the game folder again or run the setup again." : "Run the builder or the setup again.")); }
        else if (!want_sz || !want_h || sz != (long long)want_sz->num() || hx != want_h->str()) { ++bad_files; o.bad("The " + label + " changed after the build. Run the builder or the setup again."); }
      }
    }
  }
  const hle::Json* inst = have_data ? data.get("install") : nullptr;
  const hle::Json* known = inst ? inst->get("runtime_files") : nullptr;
  if (known && known->type == hle::Json::Obj) {
    for (auto& kv : known->o) {
      long long sz = 0; std::string hx;
      if (!fs::is_regular_file(exe_dir / fs::u8path(kv.first), ec)) continue;   // a missing shared DLL shows up through the record
      ++checked;
      if (!file_sha(exe_dir / fs::u8path(kv.first), &sz, &hx)) { ++bad_files; o.bad(kv.first + " cannot be read."); continue; }
      bool match = false;
      for (auto& h : kv.second.a) if (h.str() == hx) match = true;
      if (!match) { ++bad_files; o.bad(kv.first + " is not the file that ships with this build (it is modified, damaged or a different version). Run the builder or the setup again to restore it."); }
    }
  }
  if (!bad_files && checked) o.ok("built program folder: " + std::to_string(checked) + " files match.");

  // ---- summary
  Report& r = o.r;
  r.ok = o.problems == 0 && !r.lines.empty();
  size_t oks = 0;
  for (auto& l : r.lines) if (l.rfind("ok:", 0) == 0) ++oks;
  if (o.problems == 0 && oks == 0) {
    r.ok = false;
    r.summary = "Install check: nothing could be checked (the build's check files are missing). Run the builder or the setup again.";
  } else if (r.ok) {
    r.summary = "Install check: everything matches (game version 2.0.1).";
  } else {
    r.summary = "Install check: " + std::to_string(o.problems) + (o.problems == 1 ? " problem" : " problems") + " found. Details below.";
  }
  return r;
}

std::string text(const Report& r) {
  std::string t = r.summary + "\n";
  for (auto& l : r.lines) t += "  " + l + "\n";
  return t;
}

}  // namespace snap_install

extern "C" int snap_install_check_text(char* out, int cap) {
  if (!out || cap <= 0) return 0;
  wchar_t mp[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, mp, (DWORD)(sizeof mp / sizeof mp[0]));
  const std::string t = snap_install::text(snap_install::run(fs::path(mp).parent_path()));
  const int n = (int)std::min<size_t>(t.size(), (size_t)cap - 1);
  std::memcpy(out, t.data(), (size_t)n);
  out[n] = 0;
  return n;
}
