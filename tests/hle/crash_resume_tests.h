// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for crash resume (host/crash_resume.h, docs/crash-resume.md). Included by hle_tests.cpp after save_backup_tests.h (uses its helpers).
#pragma once
#include "../../host/crash_resume.h"

namespace cr_test {
namespace sbt = sb_test;
using sb_test::live_root; using sb_test::put_version; using sb_test::Small; using sb_test::cur_gen;
using sb_test::u8;
namespace sg = hle::savegen;
namespace cr = snap_resume;

inline void test_marker_roundtrip() {
  cr::Session s;
  s.pid = 4242; s.start = 133800000000000000ull; s.scene = "map_select"; s.when = 1795000000;
  cr::Session o;
  CHECK(cr::parse(cr::format(s), &o));
  CHECK_EQ(o.pid, 4242u);
  CHECK(o.start == s.start && o.scene == "map_select" && o.when == 1795000000);
  CHECK(cr::parse("pid=7\r\n", &o) && o.pid == 7 && o.scene.empty());          // an older or shorter marker
  CHECK(cr::parse("pid=7\nscene=title\n", &o) && o.scene == "title");           // bare LF
  CHECK(!cr::parse("", &o));                                                    // torn: no owner
  CHECK(!cr::parse("pid=\r\nscene=x\r\n", &o));
  CHECK(!cr::parse("pid=12x\r\n", &o));
  CHECK(!cr::parse("xpid=5\r\n", &o));                                          // the key must start a line
  CHECK(cr::parse("pid=7\r\nscene=a\x01" "b\r\n", &o) && o.scene == "ab");         // control bytes never reach a dialog
}

inline void test_targets_and_script() {
  CHECK(std::string(cr::target_for("")) == "");                // never got past the start
  CHECK(std::string(cr::target_for("title")) == "");
  CHECK(std::string(cr::target_for("main_menu")) == "main_menu");
  CHECK(std::string(cr::target_for("option")) == "main_menu");
  CHECK(std::string(cr::target_for("pause_menu")) == "main_menu");
  CHECK(std::string(cr::target_for("map_select")) == "map_select");
  CHECK(std::string(cr::target_for("photo_submit")) == "map_select");
  CHECK(std::string(cr::target_for("evaluation")) == "map_select");
  CHECK(cr::script_for("").empty());
  const std::string menu = cr::script_for("main_menu"), map = cr::script_for("map_select");
  CHECK(menu.find("wait ui=main_menu") != std::string::npos && menu.find("wait ui=map_select") == std::string::npos);
  CHECK(map.find("wait ui=main_menu") != std::string::npos && map.find("wait ui=map_select") != std::string::npos);
  CHECK(map.find("wait ui=main_menu") < map.find("wait ui=map_select"));
  for (const std::string& sc : {menu, map}) CHECK(sc.find("\nend") == std::string::npos);   // no end line: the game keeps running after the script
  CHECK(!cr::scene_label("map_select").empty() && cr::scene_label("").empty());
}

inline void test_save_flagged() {
  Small sm;
  auto root = live_root("cr_a");
  std::string why;
  CHECK(!hle::savebackup::save_flagged(u8(root), &why));                        // no save folder yet
  stdfs::create_directories(root / sbt::kUid);
  CHECK(!hle::savebackup::save_flagged(u8(root), &why));                        // an empty user folder is not a save
  put_version(root, 41);
  CHECK(!hle::savebackup::save_flagged(u8(root), &why));                        // a committed save
  const auto uid = root / sbt::kUid;
  // a kill in the middle of a commit: the new generation is written but CURRENT still names the old one, and a CURRENT.tmp is left
  const uint64_t g = cur_gen(uid);
  stdfs::create_directories(uid / "gens" / std::to_string(g + 1));
  { std::ofstream(uid / "CURRENT.tmp", std::ios::binary) << sg::pointer_text(g + 1); }
  CHECK(!hle::savebackup::save_flagged(u8(root), &why));                        // CURRENT is intact and valid: nothing is lost, the mount discards the unpublished one
  // CURRENT torn (zero bytes, as a power cut can leave it): a scan still finds the committed generation
  { std::ofstream(uid / "CURRENT", std::ios::binary | std::ios::trunc); }
  stdfs::remove(uid / "CURRENT.tmp");
  CHECK(!hle::savebackup::save_flagged(u8(root), &why));
  // CURRENT names a generation that is gone while an older one exists: flagged
  put_version(root, 42);
  const uint64_t g2 = cur_gen(uid);
  stdfs::remove(sg::gen_ok(uid, g2));
  CHECK(hle::savebackup::save_flagged(u8(root), &why));   // the scan falls back to an older generation and the pointer is stale
  // nothing loadable at all: flagged, with a reason
  auto bad = live_root("cr_b");
  stdfs::create_directories(bad / sbt::kUid / "gens" / "1");        // a generation with no .ok
  { std::ofstream(bad / sbt::kUid / "CURRENT", std::ios::binary) << "garbage"; }
  CHECK(hle::savebackup::save_flagged(u8(bad), &why) && !why.empty());
}

}  // namespace cr_test
