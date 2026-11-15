// SPDX-License-Identifier: MIT
// Tests for save import / export (runtime/hle/save_port.cpp). Included by hle_tests.cpp (uses its CHECK macros and g_tmp).
// Synthetic files only: the expected-file table is swapped for one with small sizes, except in sp_real_table (sparse files).
#pragma once
#include "../../runtime/hle/save_port.h"
#ifdef _WIN32
#include <winioctl.h>
#endif

namespace sp_test {
namespace sp = hle::saveport;
namespace sg = hle::savegen;

// Small table: same names, tiny sizes. Two of the files differ in the "old" layout.
inline const std::vector<sp::FileSpec>& small_table() {
  static std::vector<sp::FileSpec> v;
  if (v.empty()) {
    uint64_t k = 0;
    for (const auto& f : sp::expected_files()) {
      uint64_t sz = 100 + 37 * k++;
      v.push_back({f.name, sz, sz});
    }
    v[1].size = 70000; v[1].old_size = 35000;   // beyond the 32 KB inflate window
    v[12].size = 4000; v[12].old_size = 2000;
  }
  return v;
}

inline std::string content(const std::string& name, uint64_t size, int salt) {
  std::string s((size_t)size, '\0');
  uint32_t x = 2166136261u;
  for (char c : name) x = (x ^ (uint8_t)c) * 16777619u;
  x += (uint32_t)salt * 7919u;
  for (size_t i = 0; i < s.size(); ++i) {
    x = x * 1664525u + 1013904223u;
    s[i] = (i / 64) % 3 == 0 ? (char)(x >> 24) : (char)('a' + (i / 64) % 7);   // some noise, some repeats: both deflate paths
  }
  return s;
}
inline void put(const stdfs::path& p, const std::string& d) {
  stdfs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f.write(d.data(), (std::streamsize)d.size());
}
inline std::string get(const stdfs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
// The game's 15 files, sizes from `table`, in `dir`. old = use the old sizes.
inline void make_save(const stdfs::path& dir, int salt, bool old = false) {
  for (const auto& f : sp::expected_files()) put(dir / f.name, content(f.name, old ? f.old_size : f.size, salt));
}
inline bool same_save(const stdfs::path& a, const stdfs::path& b) {
  for (const auto& f : sp::expected_files()) if (get(a / f.name) != get(b / f.name)) return false;
  return true;
}
inline stdfs::path fresh(const char* name) {
  stdfs::path p = g_tmp / "sp" / name;
  std::error_code ec;
  stdfs::remove_all(p, ec);
  stdfs::create_directories(p);
  return p;
}
inline std::string u8(const stdfs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }
inline bool make_zip(const stdfs::path& dir, const stdfs::path& zip) {
  stdfs::remove(zip);
  std::string cmd = "\"\"C:\\Windows\\System32\\tar.exe\" -a -c -f \"" + zip.string() + "\" -C \"" + dir.string() + "\" . 2>nul\"";
  return std::system(cmd.c_str()) == 0 && stdfs::exists(zip);
}
inline uint64_t cur_gen(const stdfs::path& uid) { uint64_t n = 0; sg::committed_gen(uid, &n); return n; }
inline bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// Swaps in the small table for the scope of a test.
struct Small {
  Small() { sp::set_expected_files_for_test(&small_table()); }
  ~Small() { sp::set_expected_files_for_test(nullptr); }
};

inline void test_folder() {
  Small sm;
  auto src = fresh("f_src"), root = fresh("f_root");
  make_save(src, 1);
  sp::Report r; std::string err;
  CHECK(sp::import_save(u8(src), u8(root), "", &r, &err));
  CHECK(err.empty());
  auto uid = root / "534e4150524543310000000000000001";   // no save yet: the default uid
  CHECK(stdfs::exists(uid / "CURRENT"));
  uint64_t n = cur_gen(uid);
  CHECK(n >= 1 && n == r.generation);
  CHECK(same_save(src, sg::gen_dir(uid, n)));
  CHECK(r.warnings.empty());
  CHECK(!r.had_previous);
  CHECK(!r.old_version);
}

inline void test_zip_and_nested() {
  Small sm;
  // plain zip of the files, a JKSV-style zip with a date folder, and deflate through the 32 KB window
  auto a = fresh("z_a"), b = fresh("z_b");
  make_save(a, 2);
  make_save(b / "2026.11.14 - 10.30.00", 3);
  for (int variant = 0; variant < 2; ++variant) {
    auto src = variant ? b : a;
    auto zip = g_tmp / "sp" / (variant ? "z_b.zip" : "z_a.zip");
    CHECK(make_zip(src, zip));
    auto root = fresh(variant ? "z_root_b" : "z_root_a");
    sp::Report r; std::string err;
    CHECK(sp::import_save(u8(zip), u8(root), "", &r, &err));
    CHECK(err.empty());
    auto uid = root / "534e4150524543310000000000000001";
    stdfs::path want = variant ? b / "2026.11.14 - 10.30.00" : a;
    CHECK(same_save(want, sg::gen_dir(uid, cur_gen(uid))));
    if (variant) CHECK(has(r.source, "2026.11.14"));
  }
  // a damaged zip is refused and writes nothing
  auto zip = g_tmp / "sp" / "z_a.zip";
  std::string z = get(zip);
  z.resize(z.size() / 2);
  put(g_tmp / "sp" / "z_bad.zip", z);
  auto root = fresh("z_root_bad");
  sp::Report r; std::string err;
  CHECK(!sp::import_save(u8(g_tmp / "sp" / "z_bad.zip"), u8(root), "", &r, &err));
  CHECK(!err.empty());
  CHECK(!stdfs::exists(root / "534e4150524543310000000000000001" / "CURRENT"));
  // not a zip at all
  put(g_tmp / "sp" / "x.txt", "hello");
  CHECK(!sp::import_save(u8(g_tmp / "sp" / "x.txt"), u8(root), "", &r, &err));
}

inline void test_nested_folder() {
  Small sm;
  auto src = fresh("n_src"), root = fresh("n_root");
  make_save(src / "Pokemon Snap" / "2026-11-14_10-30-00", 4);
  put(src / "Pokemon Snap" / "2026-11-14_10-30-00" / "notes.txt", "x");
  sp::Report r; std::string err;
  CHECK(sp::import_save(u8(src), u8(root), "", &r, &err));
  auto uid = root / "534e4150524543310000000000000001";
  CHECK(same_save(src / "Pokemon Snap" / "2026-11-14_10-30-00", sg::gen_dir(uid, cur_gen(uid))));
  CHECK(r.warnings.size() == 1 && has(r.warnings[0], "left out"));
  CHECK(!stdfs::exists(sg::gen_dir(uid, cur_gen(uid)) / "notes.txt"));
}

inline void test_ryujinx_layout() {
  Small sm;
  auto src = fresh("r_src");
  auto id = src / "0000000000000007";
  make_save(id / "0", 5);     // committed
  make_save(id / "1", 6);     // working copy: must not be taken
  for (int variant = 0; variant < 2; ++variant) {
    auto root = fresh(variant ? "r_root1" : "r_root0");
    sp::Report r; std::string err;
    CHECK(sp::import_save(u8(variant ? id : src), u8(root), "", &r, &err));
    auto uid = root / "534e4150524543310000000000000001";
    CHECK(same_save(id / "0", sg::gen_dir(uid, cur_gen(uid))));
    CHECK(!same_save(id / "1", sg::gen_dir(uid, cur_gen(uid))));
    CHECK(has(r.source, "0"));
  }
  // the zip of the save id folder takes 0 too
  CHECK(make_zip(id, g_tmp / "sp" / "r.zip"));
  auto root = fresh("r_rootz");
  sp::Report r; std::string err;
  CHECK(sp::import_save(u8(g_tmp / "sp" / "r.zip"), u8(root), "", &r, &err));
  auto uid = root / "534e4150524543310000000000000001";
  CHECK(same_save(id / "0", sg::gen_dir(uid, cur_gen(uid))));
}

inline void test_yuzu_layout() {
  Small sm;
  auto emu = fresh("y_emu");
  auto title = emu / "nand" / "user" / "save" / "0000000000000000" / "0123456789ABCDEF0123456789ABCDEF" / sp::kTitleId;
  make_save(title, 7);
  // the title folder, the user folder above it, and the whole `save` folder all work
  for (const stdfs::path& pick : {title, title.parent_path(), title.parent_path().parent_path()}) {
    auto root = fresh("y_root");
    sp::Report r; std::string err;
    CHECK(sp::import_save(u8(pick), u8(root), "", &r, &err));
    auto uid = root / "534e4150524543310000000000000001";
    CHECK(same_save(title, sg::gen_dir(uid, cur_gen(uid))));
  }
}

inline void test_rejects() {
  Small sm;
  auto root = fresh("x_root");
  sp::Report r; std::string err;
  auto nothing = [&] { return !stdfs::exists(root / "534e4150524543310000000000000001" / "CURRENT"); };
  // another game's save
  auto other = fresh("x_other");
  put(other / "main", "abc"); put(other / "slot_1.sav", "def"); put(other / "0123456789abcdef", "ghi");
  CHECK(!sp::import_save(u8(other), u8(root), "", &r, &err));
  CHECK(has(err, "not hold New Pokemon Snap save files"));
  CHECK(nothing());
  // an empty folder, an empty zip, a missing path
  auto empty = fresh("x_empty");
  err.clear();
  CHECK(!sp::import_save(u8(empty), u8(root), "", &r, &err));
  CHECK(has(err, "no files"));
  const uint8_t eocd[22] = {0x50, 0x4b, 5, 6};
  put(g_tmp / "sp" / "x_empty.zip", std::string((const char*)eocd, 22));
  err.clear();
  CHECK(!sp::import_save(u8(g_tmp / "sp" / "x_empty.zip"), u8(root), "", &r, &err));
  CHECK(has(err, "no files"));
  err.clear();
  CHECK(!sp::import_save(u8(g_tmp / "sp" / "nope"), u8(root), "", &r, &err));
  CHECK(has(err, "does not exist"));
  // incomplete
  auto part = fresh("x_part");
  make_save(part, 8);
  stdfs::remove(part / sp::expected_files()[3].name);
  err.clear();
  CHECK(!sp::import_save(u8(part), u8(root), "", &r, &err));
  CHECK(has(err, "incomplete") && has(err, "14 of 15"));
  // right names, wrong size
  auto bad = fresh("x_bad");
  make_save(bad, 9);
  put(bad / sp::expected_files()[5].name, "short");
  err.clear();
  CHECK(!sp::import_save(u8(bad), u8(root), "", &r, &err));
  CHECK(has(err, "sizes do not"));
  // old and new sizes mixed
  auto mix = fresh("x_mix");
  make_save(mix, 10);
  put(mix / sp::expected_files()[1].name, content(sp::expected_files()[1].name, sp::expected_files()[1].old_size, 1));
  err.clear();
  CHECK(!sp::import_save(u8(mix), u8(root), "", &r, &err));
  CHECK(has(err, "mix"));
  CHECK(nothing());
}

inline void test_old_version_warns() {
  Small sm;
  auto src = fresh("o_src"), root = fresh("o_root");
  make_save(src, 11, true);
  sp::Report r; std::string err;
  CHECK(sp::import_save(u8(src), u8(root), "", &r, &err));   // warns, does not block
  CHECK(r.old_version);
  CHECK(r.warnings.size() == 1 && has(r.warnings[0], "older game version"));
  auto uid = root / "534e4150524543310000000000000001";
  CHECK(same_save(src, sg::gen_dir(uid, cur_gen(uid))));
}

inline void test_backup_preserved() {
  Small sm;
  auto root = fresh("b_root");
  auto uid = root / "534e4150524543310000000000000001";
  std::vector<stdfs::path> srcs;
  for (int i = 0; i < 8; ++i) {
    auto s = fresh(("b_src" + std::to_string(i)).c_str());
    make_save(s, 20 + i);
    srcs.push_back(s);
  }
  uint64_t prev_gen = 0;
  for (int i = 0; i < 8; ++i) {
    sp::Report r; std::string err;
    CHECK(sp::import_save(u8(srcs[i]), u8(root), "", &r, &err));
    CHECK(r.had_previous == (i > 0));
    if (i > 0) {
      CHECK_EQ(r.previous, prev_gen);
      // the save that was current is still a complete generation with its own bytes
      CHECK(sg::gen_valid(uid, prev_gen));
      CHECK(same_save(srcs[i - 1], sg::gen_dir(uid, prev_gen)));
    }
    CHECK(r.generation > prev_gen);
    prev_gen = r.generation;
    CHECK_EQ(cur_gen(uid), r.generation);
  }
  int valid = 0;
  for (auto& e : stdfs::directory_iterator(uid / "gens")) if (e.path().extension() == ".ok") ++valid;
  CHECK_EQ(valid, 1 + sp::kKeepBackups);   // the new one plus 5 backups, the rest pruned
  CHECK(!stdfs::exists(uid / "CURRENT.tmp"));
  // an import into a store the game wrote to: the game's generation stays a backup too
  auto root2 = fresh("b_root2");
  auto uid2 = root2 / "534e4150524543310000000000000001";
  make_save(sg::gen_dir(uid2, 24), 99);
  put(sg::gen_ok(uid2, 24), "ok\n");
  put(uid2 / "CURRENT", sg::pointer_text(24));
  sp::Report r; std::string err;
  CHECK(sp::import_save(u8(srcs[0]), u8(root2), "", &r, &err));
  CHECK_EQ(r.previous, 24u);
  CHECK(r.had_previous);
  CHECK_EQ(cur_gen(uid2), 25u);
  CHECK(sg::gen_valid(uid2, 24));
  stdfs::path g24 = sg::gen_dir(uid2, 24);
  for (const auto& f : sp::expected_files()) CHECK(get(g24 / f.name) == content(f.name, f.size, 99));
}

#ifdef _WIN32
inline void test_refuse_running() {
  Small sm;
  auto src = fresh("l_src"), root = fresh("l_root");
  make_save(src, 12);
  // a running game holds an exclusive byte lock on <save_root>\.snap.lock (fs_core mount_save)
  HANDLE h = CreateFileW((root / ".snap.lock").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  OVERLAPPED ov{};
  CHECK(h != INVALID_HANDLE_VALUE && LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov));
  sp::Report r; std::string err; bool running = false;
  CHECK(!sp::import_save(u8(src), u8(root), "", &r, &err, &running));
  CHECK(running);
  CHECK(has(err, "running"));
  CHECK(!stdfs::exists(root / "534e4150524543310000000000000001" / "CURRENT"));
  CloseHandle(h);
  CHECK(sp::import_save(u8(src), u8(root), "", &r, &err, &running));   // released: works
  CHECK(!running);
}
#endif

inline void test_export() {
  Small sm;
  auto src = fresh("e_src"), root = fresh("e_root");
  make_save(src, 13);
  sp::Report r; std::string err;
  CHECK(!sp::export_save(u8(root), "", u8(g_tmp / "sp" / "e_out0"), &r, &err));   // nothing to export yet
  CHECK(has(err, "no save"));
  CHECK(sp::import_save(u8(src), u8(root), "", &r, &err));
  // new folder: files directly in it
  auto out = g_tmp / "sp" / "e_out1";
  CHECK(sp::export_save(u8(root), "", u8(out), &r, &err));
  CHECK(same_save(src, out));
  CHECK_EQ(r.files, 15);
  int n = 0;
  for (auto& e : stdfs::directory_iterator(out)) { (void)e; ++n; }
  CHECK_EQ(n, 15);   // only the save files: nothing extra a restore tool would copy into the save
  // non-empty folder: a dated sub folder, the old content untouched
  auto out2 = fresh("e_out2");
  put(out2 / "keep.txt", "k");
  CHECK(sp::export_save(u8(root), "", u8(out2), &r, &err));
  CHECK(stdfs::exists(out2 / "keep.txt"));
  CHECK(r.dest != u8(out2));
  CHECK(same_save(src, stdfs::u8path(r.dest)));
  // export, then import the export into another store: same bytes (the round trip)
  auto root2 = fresh("e_root2");
  CHECK(sp::import_save(u8(out), u8(root2), "", &r, &err));
  auto uid2 = root2 / "534e4150524543310000000000000001";
  CHECK(same_save(src, sg::gen_dir(uid2, cur_gen(uid2))));
  // the export reads what the game has current, after a later import
  auto src2 = fresh("e_src2");
  make_save(src2, 14);
  CHECK(sp::import_save(u8(src2), u8(root), "", &r, &err));
  auto out3 = g_tmp / "sp" / "e_out3";
  CHECK(sp::export_save(u8(root), "", u8(out3), &r, &err));
  CHECK(same_save(src2, out3));
  CHECK(!same_save(src, out3));
}

inline void test_zip_export() {
  Small sm;
  auto src = fresh("ze_src"), root = fresh("ze_root");
  make_save(src, 31);
  sp::Report r; std::string err;
  CHECK(sp::import_save(u8(src), u8(root), "", &r, &err));
  auto zip = g_tmp / "sp" / "ze_out" / "save.zip";   // parent folder is created
  CHECK(sp::export_save(u8(root), "", u8(zip), &r, &err));
  CHECK(err.empty());
  CHECK_EQ(r.files, 15);
  CHECK(stdfs::exists(zip) && !stdfs::exists(u8(zip) + ".part"));
  // round trip: zip export, then import, byte identical
  auto root2 = fresh("ze_root2");
  CHECK(sp::import_save(u8(zip), u8(root2), "", &r, &err));
  CHECK(err.empty());
  CHECK(r.warnings.empty());
  auto uid2 = root2 / "534e4150524543310000000000000001";
  CHECK(same_save(src, sg::gen_dir(uid2, cur_gen(uid2))));
  // JKSV layout: files at the top of the zip, nothing else; an unrelated tool (bsdtar) lists and extracts it the same way
  auto ex = fresh("ze_extract");
  std::string cmd = "\"\"C:\\Windows\\System32\\tar.exe\" -x -f \"" + zip.string() + "\" -C \"" + ex.string() + "\" 2>nul\"";
  CHECK(std::system(cmd.c_str()) == 0);
  int n = 0;
  for (auto& e : stdfs::directory_iterator(ex)) { (void)e; ++n; }
  CHECK_EQ(n, 15);
  CHECK(same_save(src, ex));
  // a case-insensitive extension, overwriting an older zip, and a damaged store is reported without leaving a zip
  auto zip2 = g_tmp / "sp" / "ze_out" / "SAVE2.ZIP";
  CHECK(sp::export_save(u8(root), "", u8(zip2), &r, &err));
  CHECK(sp::export_save(u8(root), "", u8(zip2), &r, &err));
  CHECK(has(sp::format_report(r, false), "zip"));
  stdfs::remove(sg::gen_dir(root / "534e4150524543310000000000000001", cur_gen(root / "534e4150524543310000000000000001")) / sp::expected_files()[2].name);
  auto zip3 = g_tmp / "sp" / "ze_out" / "bad.zip";
  CHECK(!sp::export_save(u8(root), "", u8(zip3), &r, &err));
  CHECK(!stdfs::exists(zip3));
}

inline void test_detect() {
  Small sm;
  auto ad = fresh("d_appdata");
  make_save(ad / "Ryujinx" / "bis" / "user" / "save" / "0000000000000007" / "0", 15);
  make_save(ad / "Ryujinx" / "bis" / "user" / "save" / "0000000000000007" / "1", 16);
  put(ad / "Ryujinx" / "bis" / "user" / "save" / "0000000000000008" / "0" / "other_game_file", "x");   // another game's save
  make_save(ad / "yuzu" / "nand" / "user" / "save" / "0000000000000000" / "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" / sp::kTitleId, 17);
  make_save(ad / "eden" / "nand" / "user" / "save" / "0000000000000000" / "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB" / sp::kTitleId, 18);
  put(ad / "suyu" / "nand" / "user" / "save" / "0000000000000000" / "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC" / "0100000000010000" / "x", "x");   // another title
  auto c = sp::detect_emulator_saves(u8(ad));
  CHECK_EQ((int)c.size(), 3);
  int ryu = 0, yuzu = 0, eden = 0;
  for (auto& k : c) {
    if (k.label.rfind("Ryujinx", 0) == 0) { ++ryu; CHECK(has(k.path, "0000000000000007")); CHECK(k.mtime > 0); }
    if (k.label.rfind("yuzu", 0) == 0) ++yuzu;
    if (k.label.rfind("eden", 0) == 0) ++eden;
  }
  CHECK(ryu == 1 && yuzu == 1 && eden == 1);
  // a listed candidate imports as it is
  auto root = fresh("d_root");
  sp::Report r; std::string err;
  CHECK(sp::import_save(c[0].path, u8(root), "", &r, &err));
  CHECK(sp::detect_emulator_saves(u8(fresh("d_none"))).empty());
}

// Real table, real sizes, sparse files (no disk cost): a current and an older layout both import, and a wrong size is refused.
#ifdef _WIN32
inline void make_sparse(const stdfs::path& p, uint64_t size, char first) {
  stdfs::create_directories(p.parent_path());
  HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  DWORD got = 0;
  DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &got, nullptr);
  DWORD w = 0;
  WriteFile(h, &first, 1, &w, nullptr);
  LARGE_INTEGER li; li.QuadPart = (LONGLONG)size;
  SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
  SetEndOfFile(h);
  CloseHandle(h);
}
inline void test_real_table() {
  auto root = fresh("t_root");
  for (int old = 0; old < 2; ++old) {
    auto src = fresh(old ? "t_old" : "t_cur");
    for (const auto& f : sp::expected_files()) make_sparse(src / f.name, old ? f.old_size : f.size, (char)('A' + old));
    sp::Report r; std::string err;
    CHECK(sp::import_save(u8(src), u8(root), "", &r, &err));
    CHECK(err.empty());
    CHECK_EQ((int)r.old_version, old);
    auto uid = root / "534e4150524543310000000000000001";
    auto gd = sg::gen_dir(uid, cur_gen(uid));
    for (const auto& f : sp::expected_files()) CHECK_EQ(stdfs::file_size(gd / f.name), old ? f.old_size : f.size);
    std::error_code ec;
    stdfs::remove_all(src, ec);
  }
  CHECK_EQ(sp::expected_files().size(), (size_t)15);
  CHECK_EQ(sp::expected_files()[12].size, 1095012912ull);
  // the first byte survived the copy; one file one byte short is refused
  auto bad = fresh("t_bad");
  for (const auto& f : sp::expected_files()) make_sparse(bad / f.name, f.size - (std::string(f.name) == "bf0b4b223f011bf9" ? 1 : 0), 'Z');
  sp::Report r; std::string err;
  CHECK(!sp::import_save(u8(bad), u8(root), "", &r, &err));
  CHECK(has(err, "bf0b4b223f011bf9"));
  std::error_code ec;
  stdfs::remove_all(g_tmp / "sp", ec);
}
#endif

}  // namespace sp_test
