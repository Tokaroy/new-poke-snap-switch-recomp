// SPDX-License-Identifier: MIT
// Whole-save backups (see save_backup.h).
#include "save_backup.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <set>

#include "hle.h"
#include "hle_config.h"
#include "profiles.h"
#include "save_gen.h"

namespace hle {
namespace savebackup {

namespace fs = std::filesystem;
namespace sg = hle::savegen;

static fs::path P(const std::string& s) { return fs::u8path(s); }
static std::string S(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }

int keep_count() {
  if (const char* e = std::getenv("SNAP_SAVE_BACKUP_KEEP")) if (*e) return std::max(0, std::min(1000, std::atoi(e)));
  const int c = config().save_backup_keep;
  return c < 0 ? kDefaultKeep : std::min(c, 1000);
}

std::string backup_root(const std::string& save_root) {
  fs::path p = P(save_root);
  while (!p.empty() && (p.filename().empty() || p.filename() == ".")) p = p.parent_path();   // trailing slash
  if (p.filename() == "save") return S(p.parent_path() / "backup");
  return S(p) + "-backup";
}

// "20261115-231045-launch": 15 characters of stamp, then the reason.
static bool parse_name(const std::string& n, std::string* reason, int64_t* when) {
  if (n.size() < 17 || n[8] != '-' || n[15] != '-') return false;
  for (int i = 0; i < 15; ++i) if (i != 8 && !std::isdigit((unsigned char)n[(size_t)i])) return false;
  const std::string r = n.substr(16);
  if (r != "launch" && r != "import" && r != "restore") return false;
  std::tm t{};
  t.tm_year = std::atoi(n.substr(0, 4).c_str()) - 1900;
  t.tm_mon = std::atoi(n.substr(4, 2).c_str()) - 1;
  t.tm_mday = std::atoi(n.substr(6, 2).c_str());
  t.tm_hour = std::atoi(n.substr(9, 2).c_str());
  t.tm_min = std::atoi(n.substr(11, 2).c_str());
  t.tm_sec = std::atoi(n.substr(13, 2).c_str());
  t.tm_isdst = -1;
  if (reason) *reason = r;
  if (when) *when = (int64_t)std::mktime(&t);
  return true;
}

static std::string stamp_now(const char* reason) {
  std::time_t tt = std::time(nullptr);
  std::tm t{};
#ifdef _WIN32
  localtime_s(&t, &tt);
#else
  localtime_r(&tt, &t);
#endif
  char b[40];
  std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &t);
  return std::string(b) + "-" + reason;
}

static int64_t tree_bytes(const fs::path& d) {
  int64_t n = 0;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (it->is_regular_file(e2)) n += (int64_t)it->file_size(e2);
  }
  return n;
}

// uid folder name -> committed generation, for every user folder of a save root
static std::vector<std::pair<std::string, uint64_t>> generations(const fs::path& root) {
  std::vector<std::pair<std::string, uint64_t>> v;
  std::error_code ec;
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    uint64_t n = 0;
    if (it->is_directory(e2) && sg::committed_gen(it->path(), &n)) v.emplace_back(it->path().filename().string(), n);
  }
  std::sort(v.begin(), v.end());
  return v;
}

std::vector<Entry> list(const std::string& save_root) {
  std::vector<Entry> out;
  std::error_code ec;
  const fs::path br = P(backup_root(save_root));
  for (fs::directory_iterator it(br, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_directory(e2)) continue;
    Entry e;
    e.folder = it->path().filename().string();
    if (!parse_name(e.folder, &e.reason, &e.when)) continue;   // .tmp folders and anything else are not ours to list
    e.bytes = tree_bytes(it->path());
    out.push_back(e);
  }
  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.folder > b.folder; });
  return out;
}

static void prune(const std::string& save_root, int keep) {
  if (keep <= 0) return;
  int launch = 0, safety = 0;
  std::error_code ec;
  for (const Entry& e : list(save_root)) {   // newest first
    int& n = e.reason == "launch" ? launch : safety;
    if (++n <= keep) continue;
    const fs::path d = P(backup_root(save_root)) / e.folder;
    fs::remove_all(d, ec);
    log("save backup: removed the oldest backup %s (keeping %d)", e.folder.c_str(), keep);
  }
}

bool make(const std::string& save_root, const char* reason, std::string* made, std::string* err, int keep) {
  if (made) made->clear();
  if (keep < 0) keep = keep_count();
  if (keep == 0 || save_root.empty()) return true;
  std::string e2;
  std::string& E = err ? *err : e2;
  if (!profiles::looks_like_save(save_root)) return true;   // nothing saved yet
  const auto live = generations(P(save_root));
  if (live.empty()) return true;
  const auto have = list(save_root);
  if (!have.empty() && generations(P(backup_root(save_root)) / have.front().folder) == live) return true;   // the newest copy is current
  std::error_code ec;
  const fs::path br = P(backup_root(save_root));
  fs::create_directories(br, ec);
  if (ec) { E = "cannot create " + S(br) + ": " + ec.message(); return false; }
  std::string name = stamp_now(reason);
  fs::path fin = br / name;
  if (fs::exists(fin, ec)) return true;   // one taken this second for the same reason already
  fs::path tmp = br / (name + ".tmp");
  fs::remove_all(tmp, ec);
  if (!profiles::copy_consistent(save_root, S(tmp), &E)) {
    fs::remove_all(tmp, ec);
    if (E.empty()) E = "the copy failed";
    return false;
  }
  fs::rename(tmp, fin, ec);
  if (ec) { E = "cannot finish " + name + ": " + ec.message(); fs::remove_all(tmp, ec); return false; }
  if (made) *made = name;
  prune(save_root, keep);
  return true;
}

std::string describe(const Entry& e) {
  const char* why = e.reason == "launch" ? "at start" : e.reason == "import" ? "before an import" : "before a restore";
  return profiles::format_time(e.when) + " (" + why + ")";
}

bool restore(const std::string& save_root, const std::string& folder, saveport::Report* rep, std::string* err, bool* refused_running) {
  std::string e2;
  std::string& E = err ? *err : e2;
  if (refused_running) *refused_running = false;
  if (folder.find_first_of("/\\") != std::string::npos || !parse_name(folder, nullptr, nullptr)) { E = "That is not one of the backups."; return false; }
  const fs::path src = P(backup_root(save_root)) / folder;
  std::error_code ec;
  if (!fs::is_directory(src, ec)) { E = "That backup is gone."; return false; }
  const auto gens = generations(src);
  if (gens.empty()) { E = "That backup holds no save."; return false; }
  std::string made, be;
  if (!make(save_root, "restore", &made, &be, std::max(1, keep_count()))) {   // a restore is the one case that always keeps a safety copy
    E = "The current save could not be backed up first (" + be + "), so nothing was changed.";
    return false;
  }
  bool all = true;
  for (const auto& g : gens) {
    saveport::Report r;
    std::string ie;
    bool running = false;
    if (!saveport::import_save(S(sg::gen_dir(src / P(g.first), g.second)), save_root, g.first, &r, &ie, &running)) {
      if (refused_running && running) *refused_running = true;
      E = ie;
      all = false;
      break;
    }
    if (rep) *rep = r;
  }
  return all;
}

void backup_at_launch() {
  const char* sd = std::getenv("SNAP_SAVE_DIR");
  const char* force = std::getenv("SNAP_SAVE_BACKUP");
  if (sd && *sd && !(force && *force == '1')) return;   // test runs keep their own folders
  const int keep = keep_count();
  if (keep == 0) { log("save backup: off (save_backup_keep 0)"); return; }
  const std::string root = config().save_root;
  std::string made, err;
  if (!make(root, "launch", &made, &err, keep)) { log("save backup: FAILED for %s: %s (the game starts anyway)", root.c_str(), err.c_str()); return; }
  const size_t n = list(root).size();
  if (!made.empty()) log("save backup: %s (%zu kept, newest %d per kind)", made.c_str(), n, keep);
  else log("save backup: nothing new to copy (%zu kept)", n);
}

}  // namespace savebackup
}  // namespace hle
