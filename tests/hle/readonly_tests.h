// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the unwritable-exe-folder rules (host/readonly_logic.h). Included by hle_tests.cpp (uses its CHECK macros and g_tmp).
// The unwritable folder is a real one: an ACL deny on a folder the test makes itself, removed again at the end (Windows only).
#pragma once
#include <cstdlib>
#include "../../host/readonly_logic.h"

namespace ro_test {
namespace ro = snap_ro;

inline stdfs::path fresh(const char* name) {
  stdfs::path p = g_tmp / "ro" / name;
  std::error_code ec;
  stdfs::remove_all(p, ec);
  stdfs::create_directories(p);
  return p;
}

inline void test_probe() {
  std::string why;
  auto d = fresh("probe_ok");
  CHECK(ro::probe_writable(d, &why));
  CHECK(stdfs::is_empty(d));                       // the probe cleans up after itself
  CHECK(!ro::probe_writable(d / "missing", &why) && !why.empty());
  { std::ofstream(d / "f.txt") << "x"; }           // a file where a folder is expected
  CHECK(!ro::probe_writable(d / "f.txt", &why));
#ifdef _WIN32
  // Real refusal: deny the current user file and folder creation on a folder of our own. The ACL says deny, the write is refused.
  auto lock = fresh("probe_denied");
  const char* user = std::getenv("USERNAME");
  if (!user || !*user) { std::printf("  (no USERNAME: ACL test skipped)\n"); return; }
  const std::string grant = std::string("icacls \"") + lock.string() + "\" /deny \"" + user + ":(WD,AD)\" >nul 2>&1";
  if (std::system(grant.c_str()) != 0) { std::printf("  (icacls deny failed: ACL test skipped)\n"); return; }
  const bool denied = !ro::probe_writable(lock, &why);
  const std::string undo = std::string("icacls \"") + lock.string() + "\" /remove:d \"" + user + "\" >nul 2>&1";
  std::system(undo.c_str());
  CHECK(denied);
  CHECK(!why.empty());
  CHECK(ro::probe_writable(lock, &why));           // after the deny is removed the same folder is writable again
#endif
}

inline void test_pointer() {
  const std::string exe = "C:\\Program Files\\Snap Recomp\\work\\out\\SnapRecomp.exe";
  CHECK(ro::pointer_lookup("", exe).empty());
  std::string t = ro::pointer_update("", exe, "D:\\Data\\Snap one");
  CHECK(ro::pointer_lookup(t, exe) == "D:\\Data\\Snap one");
  CHECK(ro::pointer_lookup(t, "c:/program files/snap recomp/work/out/snaprecomp.exe") == "D:\\Data\\Snap one");   // case and slashes do not matter
  CHECK(ro::pointer_lookup(t, "C:\\Program Files\\Other\\SnapRecomp.exe").empty());                              // another exe has no entry
  t = ro::pointer_update(t, "E:\\Game\\SnapRecomp.exe", "F:\\X");
  t = ro::pointer_update(t, exe, "G:\\Moved");                                                                    // replaced, not duplicated
  CHECK(ro::pointer_lookup(t, exe) == "G:\\Moved" && ro::pointer_lookup(t, "E:\\Game\\SnapRecomp.exe") == "F:\\X");
  CHECK(std::count(t.begin(), t.end(), '\n') == 2);
  t = ro::pointer_update(t, exe, "");                                                                             // an empty folder removes it
  CHECK(ro::pointer_lookup(t, exe).empty() && ro::pointer_lookup(t, "E:\\Game\\SnapRecomp.exe") == "F:\\X");
  CHECK(ro::pointer_lookup("c:\\a\\x.exe\tD:\\d\r\n", "C:\\a\\x.exe") == "D:\\d");                               // a CRLF file
  CHECK(ro::pointer_lookup("c:\\a\\x.exe.bak\tD:\\d\n", "C:\\a\\x.exe").empty());                                // a longer key is not a match
  CHECK(ro::pointer_update("", exe, "D:\\bad\tname").empty());                                                    // a tab in the folder is refused
}

inline void test_resolve() {
  ro::Inputs in;
  in.managed_layout = true;
  in.exe_dir_writable = false;
  in.interactive = true;
  CHECK(ro::resolve(in).action == ro::Action::Ask);
  in.interactive = false;
  CHECK(ro::resolve(in).action == ro::Action::Fail);                       // --play, an action or --no-prompt: no window, an error and a nonzero exit
  in.pointer_dir = "D:\\Data";
  in.pointer_dir_writable = true;
  auto p = ro::resolve(in);
  CHECK(p.action == ro::Action::UseDataDir && p.data_dir == "D:\\Data");   // a remembered folder: silent, also without a window
  in.interactive = true;
  CHECK(ro::resolve(in).action == ro::Action::UseDataDir);
  in.pointer_dir_writable = false;                                         // the remembered folder went away: ask again
  CHECK(ro::resolve(in).action == ro::Action::Ask);
  in.interactive = false;
  CHECK(ro::resolve(in).action == ro::Action::Fail);
  in.data_dir_flag = true;                                                 // --data-dir: the caller chose, nothing is asked
  CHECK(ro::resolve(in).action == ro::Action::Normal);
  in.data_dir_flag = false;
  in.redirected_by_parent = true;
  CHECK(ro::resolve(in).action == ro::Action::Normal);
  in.redirected_by_parent = false;
  in.managed_layout = false;                                               // a development exe with no config.json or portable.txt
  CHECK(ro::resolve(in).action == ro::Action::Normal);
  in.managed_layout = true;
  in.exe_dir_writable = true;
  CHECK(ro::resolve(in).action == ro::Action::Normal);
}

inline void test_move() {
  auto root = fresh("move_src");
  stdfs::create_directories(root / "work" / "out" / "portable_data" / "Roaming");
  stdfs::create_directories(root / "tools");
  { std::ofstream(root / "work" / "out" / "SnapRecomp.exe", std::ios::binary) << std::string(200000, 'E'); }
  { std::ofstream(root / "work" / "out" / "config.json") << "{}"; }
  { std::ofstream(root / "tools" / "t.bin", std::ios::binary) << "tool"; }
  const auto exe_dir = root / "work" / "out";
  CHECK(ro::move_source(exe_dir, ro::to_u8(root), "work\\out") == root.lexically_normal());   // the whole install moves, so stored paths stay consistent
  CHECK(ro::move_source(exe_dir, "", "") == exe_dir);                                         // no install_root: the exe's own folder
  CHECK(ro::move_source(exe_dir, ro::to_u8(root), "bin") == exe_dir);                         // an exe_rel that does not match: the exe's own folder
  CHECK(ro::inside_or_same(root / "work", root) && ro::inside_or_same(root, root) && !ro::inside_or_same(root, root / "work"));
  CHECK(!ro::inside_or_same(stdfs::path(root.string() + "x"), root));                         // a sibling that shares the name prefix is not inside
  auto dst = g_tmp / "ro" / "move_dst";
  std::error_code ec;
  stdfs::remove_all(dst, ec);
  ro::CopyStats st;
  std::string why;
  CHECK(ro::copy_tree(root, dst, &st, &why));
  CHECK(st.files == 3 && st.bytes == 200000 + 2 + 4);
  CHECK(ro::verify_tree(root, dst, stdfs::path("work") / "out" / "SnapRecomp.exe", &why));
  CHECK(stdfs::is_directory(dst / "work" / "out" / "portable_data" / "Roaming"));            // empty folders are copied too
  CHECK(stdfs::exists(root / "work" / "out" / "SnapRecomp.exe"));                            // the source is untouched
  { std::ofstream(dst / "tools" / "t.bin", std::ios::binary) << "toolX"; }                    // a size difference is caught
  CHECK(!ro::verify_tree(root, dst, "", &why) && !why.empty());
  { std::ofstream(dst / "tools" / "t.bin", std::ios::binary) << "tool"; }
  { std::fstream f(dst / "work" / "out" / "SnapRecomp.exe", std::ios::binary | std::ios::in | std::ios::out); f.seekp(1000); f << 'Z'; }   // same size, one byte differs
  CHECK(ro::verify_tree(root, dst, "", &why));                                                // sizes alone match
  CHECK(!ro::verify_tree(root, dst, stdfs::path("work") / "out" / "SnapRecomp.exe", &why));   // the full compare of the exe catches it
}

}  // namespace ro_test
