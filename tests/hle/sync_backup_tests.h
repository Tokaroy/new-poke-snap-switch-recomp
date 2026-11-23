// SPDX-License-Identifier: MIT
// Tests for the profile backups to a sync folder (runtime/hle/sync_backup.cpp). Included by hle_tests.cpp after save_backup_tests.h (uses its helpers).
// Every folder is under the test's temp directory; nothing reads or writes the real user data.
#pragma once
#include "../../runtime/hle/sync_backup.h"
#include "../../runtime/hle/zip_read.h"

namespace sy_test {
namespace sy = hle::syncbackup;
namespace pr = hle::profiles;
using namespace sp_test;

// Restores the ZIP64 threshold and the corruption hook however a test ends.
struct Guard {
  ~Guard() { sy::set_zip64_threshold_for_test(0xFFFFFFFFull); sy::set_corrupt_part_for_test(false); }
};

struct Fix {
  stdfs::path base, out, photos, shots, cfgdir;
  std::string id;
  sy::Source src;
  sy::Settings set;
};

inline void put_save(const stdfs::path& save_root, int salt) {
  auto src = fresh(("sy_src" + std::to_string(salt)).c_str());
  make_save(src, salt);
  sp::Report r; std::string err;
  CHECK(sp::import_save(u8(src), u8(save_root), "", &r, &err));
}

// A profile store with one profile "Ash" holding a save, a settings.cfg, global settings, and a few photos.
inline Fix make_fix(const char* name, int salt = 1) {
  Fix f;
  f.base = fresh(name);
  const stdfs::path proot = f.base / "profiles";
  pr::Store st(u8(proot));
  std::string err;
  f.id = st.create("Ash", &err);
  CHECK(!f.id.empty());
  f.out = f.base / "sync";
  f.photos = f.base / "photos";
  f.shots = f.base / "screenshots";
  f.cfgdir = f.base / "cfg";
  f.src.profile_id = f.id;
  f.src.profile_name = "Ash";
  f.src.profiles_root = u8(proot);
  f.src.save_root = st.save_dir(f.id);
  f.src.profile_cfg = u8(proot / f.id / "settings.cfg");
  f.src.globals = {{"window.json", u8(f.cfgdir / "window.json")}, {"audio.json", u8(f.cfgdir / "audio.json")}, {"input.json", u8(f.cfgdir / "input.json")}};
  f.src.photo_dirs = {{"photos", u8(f.photos)}, {"screenshots", u8(f.shots)}};
  put(f.src.profile_cfg, "gfx_aa=smaa\n");
  put(f.cfgdir / "window.json", "{\"mode\":\"windowed\"}");
  put(f.cfgdir / "audio.json", "{}");
  put(f.photos / "photo_1.png", content("p1", 3000, 1));
  put(f.photos / "sub" / "photo_2.png", content("p2", 5000, 2));
  put(f.shots / "snap_0.png", content("s0", 1200, 3));
  put_save(f.src.save_root, salt);
  f.set.enabled = true;
  f.set.folder = u8(f.out);
  f.set.keep = 3;
  return f;
}

inline stdfs::path sg_gen(const stdfs::path& uid) { return sg::gen_dir(uid, cur_gen(uid)); }

inline int count_zips(const stdfs::path& dir) {
  int n = 0;
  std::error_code ec;
  for (auto& e : stdfs::directory_iterator(dir, ec)) if (e.path().extension() == ".zip") ++n;
  return n;
}
inline int count_parts(const stdfs::path& dir) {
  int n = 0;
  std::error_code ec;
  for (auto& e : stdfs::directory_iterator(dir, ec)) if (e.path().extension() == ".part") ++n;
  return n;
}
inline stdfs::path newest_zip(const stdfs::path& dir) {
  auto l = sy::list_backups(u8(dir));
  return l.empty() ? stdfs::path() : stdfs::path(l.front().path);
}

inline void test_sha_and_settings() {
  CHECK(sy::sha256_hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sy::sha256_hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  const std::string big(1000, 'a');   // crosses block boundaries and the padding case
  CHECK(sy::sha256_hex(big.data(), big.size()) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
  // settings: defaults, round trip, bounds, env wins for one run and is never written back
  auto dir = fresh("sy_settings");
  _putenv_s("SNAP_SYNC_BACKUP", "");
  _putenv_s("SNAP_SYNC_BACKUP_FOLDER", "");
  _putenv_s("SNAP_SYNC_BACKUP_KEEP", "");
  _putenv_s("SNAP_SYNC_BACKUP_INCLUDE", "");
  sy::Settings d = sy::read_settings(u8(dir));
  CHECK(!d.enabled && d.folder.empty() && d.keep == 3 && d.saves && d.settings && d.photos);
  sy::Settings s;
  s.enabled = true; s.folder = u8(dir / "dest"); s.keep = 7; s.photos = false;
  std::string err;
  CHECK(sy::write_settings(u8(dir), s, &err));
  sy::Settings r = sy::read_settings(u8(dir));
  CHECK(r.enabled && r.folder == s.folder && r.keep == 7 && r.saves && r.settings && !r.photos);
  s.keep = 500;
  CHECK(sy::write_settings(u8(dir), s, &err));
  CHECK_EQ(sy::read_settings(u8(dir)).keep, sy::kMaxKeep);
  s.keep = 0;
  CHECK(sy::write_settings(u8(dir), s, &err));
  CHECK_EQ(sy::read_settings(u8(dir)).keep, 1);   // at least one copy is always kept
  _putenv_s("SNAP_SYNC_BACKUP", "0");
  _putenv_s("SNAP_SYNC_BACKUP_KEEP", "5");
  _putenv_s("SNAP_SYNC_BACKUP_INCLUDE", "saves,photos");
  r = sy::read_settings(u8(dir));
  CHECK(!r.enabled && r.keep == 5 && r.saves && !r.settings && r.photos);
  CHECK(sy::read_settings(u8(dir), false).enabled);   // the file itself still says on
  _putenv_s("SNAP_SYNC_BACKUP", "");
  _putenv_s("SNAP_SYNC_BACKUP_KEEP", "");
  _putenv_s("SNAP_SYNC_BACKUP_INCLUDE", "");
  CHECK(sy::include_text(d) == "saves, settings, photos");
}

inline void test_backup_zip_and_manifest() {
  Small sm;
  Guard g;
  Fix f = make_fix("sy_a", 11);
  const sy::RunResult r = sy::run_backup(f.src, f.set, false);
  CHECK(r.outcome == sy::Outcome::Made);
  CHECK_EQ(count_zips(f.out), 1);
  CHECK_EQ(count_parts(f.out), 0);   // the temp name is gone after the rename
  const std::string zip = r.zip;
  std::vector<hle::zipr::Entry> ents;
  std::string err;
  CHECK(hle::zipr::list(zip, ents, &err, 1ull << 40));
  CHECK(!ents.empty() && ents.back().rel == "manifest.json");   // the manifest is the LAST entry
  std::set<std::string> names;
  for (auto& e : ents) names.insert(e.rel);
  CHECK(names.count("save/534e4150524543310000000000000001/CURRENT"));
  CHECK(names.count("settings/profile/settings.cfg"));
  CHECK(names.count("settings/global/window.json"));
  CHECK(names.count("photos/photos/sub/photo_2.png"));
  CHECK(names.count("photos/screenshots/snap_0.png"));
  bool has_gen = false;
  for (auto& n : names) if (n.find("/gens/") != std::string::npos && n.size() > 3 && n.compare(n.size() - 3, 3, ".ok") == 0) has_gen = true;
  CHECK(has_gen);
  for (auto& n : names) CHECK(n.find("hdpack") == std::string::npos && n.find("shader") == std::string::npos);   // rebuildable data never goes in
  CHECK(sy::verify_zip(zip, &err));
  auto l = sy::list_backups(u8(f.out), f.id);
  CHECK_EQ(l.size(), 1u);
  CHECK(l[0].valid && l[0].has_saves && l[0].has_settings && l[0].has_photos && l[0].profile_name == "Ash");
  CHECK(l[0].summary.find("save: 15 files") != std::string::npos);
  CHECK(l[0].bytes > 0 && l[0].when > 0);
  // nothing changed: no second zip; forced: one more
  CHECK(sy::run_backup(f.src, f.set, false).outcome == sy::Outcome::Unchanged);
  CHECK_EQ(count_zips(f.out), 1);
  CHECK(sy::run_backup(f.src, f.set, true).outcome == sy::Outcome::Made);
  CHECK_EQ(count_zips(f.out), 2);
  // the save changes: the next normal run makes a copy
  put_save(f.src.save_root, 12);
  CHECK(sy::run_backup(f.src, f.set, false).outcome == sy::Outcome::Made);
  // what to include: saves only
  Fix h = make_fix("sy_a2", 13);
  h.set.settings = false; h.set.photos = false;
  const sy::RunResult r2 = sy::run_backup(h.src, h.set, false);
  CHECK(r2.outcome == sy::Outcome::Made);
  ents.clear();
  CHECK(hle::zipr::list(r2.zip, ents, &err, 1ull << 40));
  for (auto& e : ents) CHECK(e.rel.compare(0, 5, "save/") == 0 || e.rel == "manifest.json");
  // a missing folder setting and an empty selection are failures with a message, not a crash
  sy::Settings none = f.set;
  none.folder.clear();
  CHECK(sy::run_backup(f.src, none, false).outcome == sy::Outcome::Failed);
  none = f.set; none.saves = none.settings = none.photos = false;
  CHECK(sy::run_backup(f.src, none, false).outcome == sy::Outcome::Failed);
}

inline void test_zip64() {
  Small sm;
  Guard g;
  sy::set_zip64_threshold_for_test(64);   // every file over 64 bytes, the archive offsets and the directory take the ZIP64 paths
  Fix f = make_fix("sy_z64", 21);
  const sy::RunResult r = sy::run_backup(f.src, f.set, false);
  CHECK(r.outcome == sy::Outcome::Made);
  std::vector<hle::zipr::Entry> ents;
  std::string err;
  CHECK(hle::zipr::list(r.zip, ents, &err, 1ull << 40));   // the reader follows the ZIP64 end record, sizes and offsets
  CHECK(ents.size() > 10);
  bool big = false;
  for (auto& e : ents) if (e.size >= 64) big = true;
  CHECK(big);
  CHECK(sy::verify_zip(r.zip, &err));
  // and a standard reader agrees (Windows tar reads ZIP64)
  const stdfs::path ex = fresh("sy_z64_x");
  std::string cmd = "\"\"C:\\Windows\\System32\\tar.exe\" -x -f \"" + r.zip + "\" -C \"" + ex.string() + "\" 2>nul\"";
  if (std::system(cmd.c_str()) == 0) CHECK(get(ex / "photos" / "photos" / "photo_1.png") == content("p1", 3000, 1));
  // a restore through the ZIP64 archive gives the same save
  sy::RestoreReport rep;
  CHECK(sy::restore_new_profile(r.zip, f.src, sy::RestoreOptions(), &rep, &err));
  pr::Store st(f.src.profiles_root);
  st.load();
  auto m = fresh("sy_z64_m");
  make_save(m, 21);
  const auto uid = stdfs::path(st.save_dir(rep.profile_id)) / "534e4150524543310000000000000001";
  CHECK(same_save(m, sg_gen(uid)));
}

inline void test_rotation() {
  Small sm;
  Guard g;
  Fix f = make_fix("sy_rot", 31);
  f.set.keep = 2;
  for (int i = 0; i < 4; ++i) {
    if (i) put_save(f.src.save_root, 31 + i);
    const sy::RunResult r = sy::run_backup(f.src, f.set, false);
    CHECK(r.outcome == sy::Outcome::Made);
    CHECK_EQ(r.removed, i >= 2 ? 1 : 0);
  }
  auto l = sy::list_backups(u8(f.out), f.id);
  CHECK_EQ(l.size(), 2u);   // the two newest remain
  CHECK(l[0].file > l[1].file);
  CHECK(l[0].signature != l[1].signature);
  CHECK_EQ(count_zips(f.out), 2);
  // another profile's copies are neither counted nor removed
  Fix o = make_fix("sy_rot_o", 40);
  o.src.profile_id = "p9"; o.src.profile_name = "Other";
  o.set.folder = f.set.folder;
  o.set.keep = 1;
  CHECK(sy::run_backup(o.src, o.set, false).outcome == sy::Outcome::Made);
  CHECK_EQ(sy::list_backups(u8(f.out), f.id).size(), 2u);
  CHECK_EQ(sy::list_backups(u8(f.out), "p9").size(), 1u);
  // a damaged zip is not counted as a copy and is never deleted by the rotation
  put(f.out / ("SnapRecomp-" + f.id + "-20200101-000000.zip"), "not a zip at all");
  put_save(f.src.save_root, 50);
  CHECK(sy::run_backup(f.src, f.set, false).outcome == sy::Outcome::Made);
  CHECK(stdfs::exists(f.out / ("SnapRecomp-" + f.id + "-20200101-000000.zip")));
  int valid = 0, bad = 0;
  for (auto& b : sy::list_backups(u8(f.out), f.id)) (b.valid ? valid : bad)++;
  CHECK_EQ(valid, 2);
  CHECK_EQ(bad, 1);
  // a new copy that fails its verification is thrown away and nothing older is deleted
  const int before = count_zips(f.out);
  const auto olds = sy::list_backups(u8(f.out), f.id);
  put_save(f.src.save_root, 51);
  sy::set_corrupt_part_for_test(true);
  const sy::RunResult bad_run = sy::run_backup(f.src, f.set, false);
  sy::set_corrupt_part_for_test(false);
  CHECK(bad_run.outcome == sy::Outcome::Failed);
  CHECK(bad_run.message.find("verify") != std::string::npos);
  CHECK_EQ(count_zips(f.out), before);
  CHECK_EQ(count_parts(f.out), 0);
  CHECK_EQ(sy::list_backups(u8(f.out), f.id).size(), olds.size());
  for (auto& b : olds) CHECK(stdfs::exists(b.path));
}

inline void test_interrupted_write() {
  Small sm;
  Guard g;
  Fix f = make_fix("sy_int", 61);
  stdfs::create_directories(f.out);
  const std::string pre = "SnapRecomp-" + f.id + "-";
  // what a killed run leaves: a .part, and a zip cut off before its directory and manifest
  put(f.out / (pre + "20250101-010101.zip.part"), content("junk", 5000, 1));
  const sy::RunResult first = sy::run_backup(f.src, f.set, true);
  CHECK(first.outcome == sy::Outcome::Made);
  CHECK_EQ(count_parts(f.out), 0);   // the leftover was cleaned up
  const std::string whole = get(first.zip);
  const stdfs::path cut = f.out / (pre + "20250202-020202.zip");
  put(cut, whole.substr(0, whole.size() / 2));
  put(f.out / (pre + "20250303-030303.zip.part"), "half");
  auto l = sy::list_backups(u8(f.out), f.id);
  CHECK_EQ(l.size(), 2u);   // the .part is never listed
  int bad = 0;
  for (auto& b : l) if (!b.valid) { ++bad; CHECK(!b.problem.empty()); }
  CHECK_EQ(bad, 1);
  std::string err;
  CHECK(!sy::verify_zip(u8(cut), &err));
  // the next run still works, counts only whole zips, and does not delete the cut one
  put_save(f.src.save_root, 62);
  f.set.keep = 1;
  const sy::RunResult next = sy::run_backup(f.src, f.set, false);
  CHECK(next.outcome == sy::Outcome::Made);
  CHECK(stdfs::exists(cut));
  CHECK_EQ(count_parts(f.out), 0);
  int valid = 0;
  for (auto& b : sy::list_backups(u8(f.out), f.id)) if (b.valid) ++valid;
  CHECK_EQ(valid, 1);
  // a run whose write goes wrong leaves no .part and no new zip
  const int zips = count_zips(f.out);
  put_save(f.src.save_root, 63);
  sy::set_corrupt_part_for_test(true);
  CHECK(sy::run_backup(f.src, f.set, false).outcome == sy::Outcome::Failed);
  sy::set_corrupt_part_for_test(false);
  CHECK_EQ(count_zips(f.out), zips);
  CHECK_EQ(count_parts(f.out), 0);
  // the folder is not a folder: a message, not a crash
  sy::Settings bad_set = f.set;
  put(f.base / "afile", "x");
  bad_set.folder = u8(f.base / "afile" / "inside");
  CHECK(sy::run_backup(f.src, bad_set, false).outcome == sy::Outcome::Failed);
}

inline void test_save_not_ready() {
  Small sm;
  Guard g;
  Fix f = make_fix("sy_ready", 71);
  {   // no save yet: saves-only has nothing to do and no zip appears
    Fix e = make_fix("sy_ready_e", 71);
    stdfs::remove_all(e.src.save_root);
    stdfs::create_directories(e.src.save_root);
    sy::Settings only = e.set;
    only.settings = only.photos = false;
    CHECK(sy::inspect_save(e.src.save_root).health == sy::Health::NoSave);
    CHECK(sy::run_backup(e.src, only, false).outcome == sy::Outcome::Unchanged);
    CHECK_EQ(count_zips(e.out), 0);
  }
  CHECK(sy::inspect_save(f.src.save_root).health == sy::Health::Ok);
  const auto uid = stdfs::path(f.src.save_root) / "534e4150524543310000000000000001";
  // mid-write: a generation without its completion marker
  stdfs::create_directories(uid / "gens" / "99");
  put(uid / "gens" / "99" / "x", "half");
  auto st = sy::inspect_save(f.src.save_root);
  CHECK(st.health == sy::Health::MidWrite && !st.detail.empty());
  sy::RunResult r = sy::run_backup(f.src, f.set, true);
  CHECK(r.outcome == sy::Outcome::Skipped);
  CHECK(r.message.find("mid-write") != std::string::npos);
  CHECK_EQ(count_zips(f.out), 0);
  stdfs::remove_all(uid / "gens" / "99");
  CHECK(sy::inspect_save(f.src.save_root).health == sy::Health::Ok);
  // an interrupted pointer replace
  put(uid / "CURRENT.tmp", get(uid / "CURRENT"));
  CHECK(sy::inspect_save(f.src.save_root).health == sy::Health::MidWrite);
  stdfs::remove(uid / "CURRENT.tmp");
  // flagged for recovery: a torn CURRENT, an orphaned generation, a queued restore
  const std::string cur = get(uid / "CURRENT");
  put(uid / "CURRENT", "garbage");
  st = sy::inspect_save(f.src.save_root);
  CHECK(st.health == sy::Health::NeedsRecovery);
  r = sy::run_backup(f.src, f.set, true);
  CHECK(r.outcome == sy::Outcome::Skipped && r.message.find("recovery") != std::string::npos);
  CHECK_EQ(count_zips(f.out), 0);
  put(uid / "CURRENT", cur);
  CHECK(sy::inspect_save(f.src.save_root).health == sy::Health::Ok);
  put(uid / "gens" / "7.orphan", "orphan\n");
  CHECK(sy::inspect_save(f.src.save_root).health == sy::Health::NeedsRecovery);
  stdfs::remove(uid / "gens" / "7.orphan");
  const stdfs::path pend = stdfs::path(hle::savebackup::backup_root(f.src.save_root)) / "restore.pending";
  put(pend, "20250101-000000-launch\n");
  CHECK(sy::inspect_save(f.src.save_root).health == sy::Health::NeedsRecovery);
  stdfs::remove(pend);
  CHECK(sy::inspect_save(f.src.save_root).health == sy::Health::Ok);
  CHECK(sy::run_backup(f.src, f.set, true).outcome == sy::Outcome::Made);   // back to normal: it backs up
#ifdef _WIN32
  // in use: a running game holds the save folder's lock
  HANDLE h = CreateFileW((stdfs::path(f.src.save_root) / ".snap.lock").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  OVERLAPPED ov{};
  CHECK(h != INVALID_HANDLE_VALUE && LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov));
  CHECK(sy::inspect_save(f.src.save_root).health == sy::Health::InUse);
  const int zips = count_zips(f.out);
  r = sy::run_backup(f.src, f.set, true);
  CHECK(r.outcome == sy::Outcome::Skipped);
  CHECK_EQ(count_zips(f.out), zips);
  sy::RestoreReport rep; std::string err; bool running = false;
  const std::string zip = sy::list_backups(u8(f.out))[0].path;
  CHECK(!sy::restore_replace_current(zip, f.src, sy::RestoreOptions(), true, &rep, &err, &running));
  CHECK(running);
  CloseHandle(h);
#endif
}

inline void test_restore_new_profile() {
  Small sm;
  Guard g;
  Fix f = make_fix("sy_rest", 81);
  const sy::RunResult made = sy::run_backup(f.src, f.set, false);
  CHECK(made.outcome == sy::Outcome::Made);
  auto A = fresh("sy_rest_A");
  make_save(A, 81);
  const auto uid = stdfs::path(f.src.save_root) / "534e4150524543310000000000000001";
  // the live profile moves on, and a photo goes missing
  put_save(f.src.save_root, 82);
  const uint64_t live_gen = cur_gen(uid);
  stdfs::remove(f.photos / "sub" / "photo_2.png");
  put(f.photos / "photo_1.png", "changed since the backup");
  put(f.src.profile_cfg, "gfx_aa=off\n");
  pr::Store before(f.src.profiles_root);
  before.load();
  const size_t n_before = before.list().size();
  sy::RestoreReport rep;
  std::string err;
  CHECK(sy::restore_new_profile(made.zip, f.src, sy::RestoreOptions(), &rep, &err));
  CHECK(err.empty());
  pr::Store st(f.src.profiles_root);
  st.load();
  CHECK_EQ(st.list().size(), n_before + 1);
  CHECK(rep.profile_id != f.id && st.find(rep.profile_id) != nullptr);
  CHECK(rep.profile_name.find("Ash") != std::string::npos && rep.profile_name.find("backup") != std::string::npos);
  // the new profile holds the save as it was at backup time, and its settings
  const auto nuid = stdfs::path(st.save_dir(rep.profile_id)) / "534e4150524543310000000000000001";
  CHECK(same_save(A, sg_gen(nuid)));
  CHECK(sg::gen_valid(nuid, cur_gen(nuid)));
  CHECK(rep.settings_restored && get(stdfs::path(f.src.profiles_root) / rep.profile_id / "settings.cfg") == "gfx_aa=smaa\n");
  // the current profile is untouched: save, settings, and the changed photo (an existing file is never overwritten)
  CHECK_EQ(cur_gen(uid), live_gen);
  auto B = fresh("sy_rest_B");
  make_save(B, 82);
  CHECK(same_save(B, sg_gen(uid)));
  CHECK(get(f.src.profile_cfg) == "gfx_aa=off\n");
  CHECK(get(f.photos / "photo_1.png") == "changed since the backup");
  CHECK_EQ(rep.photos_kept, 2);   // photo_1 and the screenshot were there
  CHECK_EQ(rep.photos_restored, 1);
  CHECK(get(f.photos / "sub" / "photo_2.png") == content("p2", 5000, 2));
  // options: no settings, no photos
  stdfs::remove(f.photos / "sub" / "photo_2.png");
  sy::RestoreOptions off; off.settings = false; off.photos = false;
  sy::RestoreReport rep2;
  CHECK(sy::restore_new_profile(made.zip, f.src, off, &rep2, &err));
  CHECK(!rep2.settings_restored && rep2.photos_restored == 0);
  CHECK(!stdfs::exists(f.photos / "sub" / "photo_2.png"));
  CHECK(rep2.profile_name != rep.profile_name);   // a second restore gets its own name
  // a damaged backup restores nothing and leaves no new profile
  std::string flipped = get(made.zip);
  flipped[flipped.find("gfx_aa=smaa")] ^= 1;   // a byte inside a stored file: the SHA-256 and CRC no longer match
  const stdfs::path bad = f.out / ("SnapRecomp-" + f.id + "-20300101-000000.zip");
  put(bad, flipped);
  pr::Store s2(f.src.profiles_root);
  s2.load();
  const size_t n2 = s2.list().size();
  CHECK(!sy::verify_zip(u8(bad), &err));
  CHECK(!sy::restore_new_profile(u8(bad), f.src, sy::RestoreOptions(), &rep2, &err));
  CHECK(err.find("did not verify") != std::string::npos);
  pr::Store s3(f.src.profiles_root);
  s3.load();
  CHECK_EQ(s3.list().size(), n2);
  // with profiles off there is no new profile to make
  sy::Source off_src = f.src;
  off_src.profiles_root.clear();
  CHECK(!sy::restore_new_profile(made.zip, off_src, sy::RestoreOptions(), &rep2, &err));
}

inline void test_restore_replace_current() {
  Small sm;
  Guard g;
  Fix f = make_fix("sy_repl", 91);
  const sy::RunResult made = sy::run_backup(f.src, f.set, false);
  CHECK(made.outcome == sy::Outcome::Made);
  auto A = fresh("sy_repl_A");
  make_save(A, 91);
  put_save(f.src.save_root, 92);
  auto B = fresh("sy_repl_B");
  make_save(B, 92);
  const auto uid = stdfs::path(f.src.save_root) / "534e4150524543310000000000000001";
  const uint64_t gen = cur_gen(uid);
  sy::RestoreReport rep;
  std::string err;
  // without the second confirmation nothing happens
  CHECK(!sy::restore_replace_current(made.zip, f.src, sy::RestoreOptions(), false, &rep, &err));
  CHECK(err.find("confirmation") != std::string::npos);
  CHECK_EQ(cur_gen(uid), gen);
  CHECK(same_save(B, sg_gen(uid)));
  // confirmed: the backup becomes current as a new generation, the old one stays, a local safety copy is taken
  CHECK(sy::restore_replace_current(made.zip, f.src, sy::RestoreOptions(), true, &rep, &err));
  CHECK(same_save(A, sg_gen(uid)));
  CHECK(cur_gen(uid) > gen);
  CHECK(sg::gen_valid(uid, gen));
  bool safety = false;
  for (auto& e : hle::savebackup::list(f.src.save_root)) if (e.reason == "restore") safety = true;
  CHECK(safety);
}

}  // namespace sy_test
