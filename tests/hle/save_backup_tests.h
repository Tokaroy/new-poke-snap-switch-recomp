// SPDX-License-Identifier: MIT
// Tests for the whole-save backups (runtime/hle/save_backup.cpp). Included by hle_tests.cpp after save_port_tests.h (uses its helpers).
#pragma once
#include "../../runtime/hle/save_backup.h"

namespace sb_test {
namespace sb = hle::savebackup;
using namespace sp_test;

inline const char* kUid = "534e4150524543310000000000000001";

// <fresh>/save is the live save root, so the backups land in <fresh>/backup
inline stdfs::path live_root(const char* name) { return fresh(name) / "save"; }

inline void put_version(const stdfs::path& root, int salt) {
  auto src = fresh(("sb_src" + std::to_string(salt)).c_str());
  make_save(src, salt);
  sp::Report r; std::string err;
  CHECK(sp::import_save(u8(src), u8(root), "", &r, &err));
}

inline void test_make_and_list() {
  Small sm;
  auto root = live_root("sb_a");
  std::string made, err;
  CHECK(sb::make(u8(root), "launch", &made, &err, 10));   // no save yet: nothing to copy, not an error
  CHECK(made.empty());
  CHECK(!stdfs::exists(sb::backup_root(u8(root))));
  put_version(root, 31);
  const auto uid = root / kUid;
  const uint64_t gen = cur_gen(uid);
  CHECK(sb::make(u8(root), "launch", &made, &err, 10));
  CHECK(!made.empty());
  auto l = sb::list(u8(root));
  CHECK_EQ(l.size(), 1u);
  CHECK(l[0].reason == "launch" && l[0].when > 0 && l[0].bytes > 0);
  CHECK(stdfs::exists(stdfs::path(sb::backup_root(u8(root))) / l[0].folder / kUid / "CURRENT"));
  // the live save is exactly as it was
  CHECK_EQ(cur_gen(uid), gen);
  CHECK(stdfs::exists(uid / "CURRENT"));
  // nothing changed since: no second copy
  CHECK(sb::make(u8(root), "launch", &made, &err, 10));
  CHECK(made.empty());
  CHECK_EQ(sb::list(u8(root)).size(), 1u);
  // the save changed: a new copy (names carry the second, so age the first one; two starts inside one second share a name)
  stdfs::rename(stdfs::path(sb::backup_root(u8(root))) / l[0].folder, stdfs::path(sb::backup_root(u8(root))) / "20260101-120000-launch");
  put_version(root, 32);
  CHECK(sb::make(u8(root), "launch", &made, &err, 10));
  CHECK(!made.empty());
  CHECK_EQ(sb::list(u8(root)).size(), 2u);
  CHECK(sb::list(u8(root))[0].folder > sb::list(u8(root))[1].folder);   // newest first
}

inline void test_prune_and_off() {
  Small sm;
  auto root = live_root("sb_b");
  std::string made, err;
  put_version(root, 40);
  CHECK(sb::make(u8(root), "import", &made, &err, 2));   // a safety copy is kept apart from the launch ones
  CHECK(!made.empty());
  // distinct names need distinct seconds: write the launch backups by hand-made folders with older stamps
  const stdfs::path br = sb::backup_root(u8(root));
  for (int i = 0; i < 4; ++i) {
    char nm[40];
    snprintf(nm, sizeof nm, "2026010%d-120000-launch", i + 1);
    stdfs::create_directories(br / nm / kUid);
    put(br / nm / kUid / "CURRENT", "x");
  }
  put_version(root, 41);
  CHECK(sb::make(u8(root), "launch", &made, &err, 2));   // keeps the 2 newest launch backups
  int launch = 0, import = 0;
  for (auto& e : sb::list(u8(root))) { if (e.reason == "launch") ++launch; if (e.reason == "import") ++import; }
  CHECK_EQ(launch, 2);
  CHECK_EQ(import, 1);   // untouched by the launch pruning
  // the live save is never part of the pruning
  CHECK(stdfs::exists(root / kUid / "CURRENT"));
  CHECK(sg::gen_valid(root / kUid, cur_gen(root / kUid)));
  // off: keep 0 makes nothing and removes nothing
  const size_t before = sb::list(u8(root)).size();
  put_version(root, 42);
  CHECK(sb::make(u8(root), "launch", &made, &err, 0));
  CHECK(made.empty());
  CHECK_EQ(sb::list(u8(root)).size(), before);
}

inline void test_restore() {
  Small sm;
  auto root = live_root("sb_c");
  std::string made, err;
  put_version(root, 50);
  auto srcA = fresh("sb_srcA");
  make_save(srcA, 50);
  CHECK(sb::make(u8(root), "launch", &made, &err, 10));
  const std::string folderA = made;
  put_version(root, 51);   // the live save moves on
  auto srcB = fresh("sb_srcB");
  make_save(srcB, 51);
  const auto uid = root / kUid;
  CHECK(same_save(srcB, sg::gen_dir(uid, cur_gen(uid))));
  sp::Report r; bool running = true;
  CHECK(sb::restore(u8(root), folderA, &r, &err, &running));
  CHECK(!running);
  CHECK(same_save(srcA, sg::gen_dir(uid, cur_gen(uid))));       // the old save is current again
  bool found_safety = false;
  for (auto& e : sb::list(u8(root))) {
    if (e.reason != "restore") continue;
    found_safety = true;   // a copy of the save from before the restore, with its own bytes
    auto bu = stdfs::path(sb::backup_root(u8(root))) / e.folder / kUid;
    uint64_t n = 0; sg::committed_gen(bu, &n);
    CHECK(same_save(srcB, sg::gen_dir(bu, n)));
  }
  CHECK(found_safety);
  CHECK(sg::gen_valid(uid, r.previous));                        // and the generation that was current is still there
  // bad names and missing backups are refused cleanly
  CHECK(!sb::restore(u8(root), "..\\..\\x", &r, &err));
  CHECK(!sb::restore(u8(root), "20250101-000000-launch", &r, &err));
  CHECK(same_save(srcA, sg::gen_dir(uid, cur_gen(uid))));
}

#ifdef _WIN32
inline void test_restore_refused_running() {
  Small sm;
  auto root = live_root("sb_d");
  std::string made, err;
  put_version(root, 60);
  CHECK(sb::make(u8(root), "launch", &made, &err, 10));
  put_version(root, 61);
  const auto uid = root / kUid;
  const uint64_t gen = cur_gen(uid);
  HANDLE h = CreateFileW((root / ".snap.lock").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  OVERLAPPED ov{};
  CHECK(h != INVALID_HANDLE_VALUE && LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov));
  sp::Report r; bool running = false;
  CHECK(!sb::restore(u8(root), made, &r, &err, &running));
  CHECK(running);
  CHECK_EQ(cur_gen(uid), gen);   // nothing changed
  CloseHandle(h);
}
#endif

}  // namespace sb_test
