// Host-only tests for runtime/mods/mods_core (path mapping, index priority, replacement-chunk size override).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "hle/zip_read.h"
#include "mods/mods_core.h"

namespace fs = std::filesystem;
static int g_fail = 0, g_n = 0;
#define CHECK(c) do { g_n++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void write(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << s;
}

static std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// A stored (uncompressed) zip of (name, data) pairs.
static void write_zip(const fs::path& p, const std::vector<std::pair<std::string, std::string>>& files) {
  std::string out, cd;
  auto u16 = [](std::string& s, uint32_t v) { s += (char)(v & 0xff); s += (char)((v >> 8) & 0xff); };
  auto u32 = [&](std::string& s, uint32_t v) { u16(s, v & 0xffff); u16(s, v >> 16); };
  for (auto& [name, data] : files) {
    uint32_t crc = mods::crc32(data.data(), data.size()), off = (uint32_t)out.size(), n = (uint32_t)data.size();
    u32(out, 0x04034b50); u16(out, 20); u16(out, 0); u16(out, 0); u16(out, 0); u16(out, 0x21);
    u32(out, crc); u32(out, n); u32(out, n); u16(out, (uint32_t)name.size()); u16(out, 0);
    out += name; out += data;
    u32(cd, 0x02014b50); u16(cd, 20); u16(cd, 20); u16(cd, 0); u16(cd, 0); u16(cd, 0); u16(cd, 0x21);
    u32(cd, crc); u32(cd, n); u32(cd, n); u16(cd, (uint32_t)name.size()); u16(cd, 0); u16(cd, 0); u16(cd, 0); u16(cd, 0);
    u32(cd, 0); u32(cd, off); cd += name;
  }
  uint32_t cdo = (uint32_t)out.size();
  out += cd;
  u32(out, 0x06054b50); u16(out, 0); u16(out, 0); u16(out, (uint32_t)files.size()); u16(out, (uint32_t)files.size());
  u32(out, (uint32_t)cd.size()); u32(out, cdo); u16(out, 0);
  write(p, out);
}

int main() {
  using namespace mods;
  // ext ids (known values from romfs_census.DRP_EXT)
  CHECK(ext_crc("nutexb") == 0x5C156DBC);
  CHECK(ext_crc("NUTEXB") == 0x5C156DBC);
  CHECK(ext_crc("msbt") == 0xBE1C9ACB);
  CHECK(ext_crc("DC4A8177") == 0xDC4A8177);
  CHECK(crc32("123456789", 9) == 0xCBF43926u);
  CHECK(norm_archive("Common\\UI/Master\\a.drp") == "common/ui/master/a.drp");
  CHECK(norm_archive("rom:/common/Text/x.drp") == "common/text/x.drp");

  std::string a, s;
  uint32_t x = 0;
  CHECK(parse_pack_path("common/UI/master/Foo.drp/Icon_A.nutexb", &a, &s, &x));
  CHECK(a == "common/ui/master/foo.drp" && s == "icon_a" && x == 0x5C156DBC);
  CHECK(parse_pack_path("a.drp/foo.nutexb.nutexb", &a, &s, &x) && s == "foo");     // extracted name that repeats its ext
  CHECK(parse_pack_path("a.drp/foo.msbt", &a, &s, &x) && s == "foo" && x == 0xBE1C9ACB);
  CHECK(!parse_pack_path("pack.json", &a, &s, &x));
  CHECK(!parse_pack_path("common/UI/foo.nutexb", &a, &s, &x));            // no .drp component
  CHECK(!parse_pack_path("a.drp/sub/foo.msbt", &a, &s, &x));              // entry must sit directly in the archive folder
  CHECK(!parse_pack_path("a.drp/noext", &a, &s, &x));

  // Texture capture paths (SNAP_TEXDUMP): pack layout, ".nutexb" added only when missing, nothing that leaves the folder
  CHECK(texdump_rel("nx/pack/characters/master/ch_097.drp", "CH_097_00_0_A00_Body_dif.nutexb") ==
        "nx/pack/characters/master/ch_097.drp/CH_097_00_0_A00_Body_dif.nutexb");
  CHECK(texdump_rel("a.drp", "icon") == "a.drp/icon.nutexb");
  {
    std::string ra, rs; uint32_t rx = 0;   // a capture path is a valid pack path for the same entry
    CHECK(parse_pack_path(texdump_rel("common/ui/x.drp", "Icon_A.nutexb"), &ra, &rs, &rx) && ra == "common/ui/x.drp" && rs == "icon_a" && rx == ext_crc("nutexb"));
  }
  CHECK(texdump_rel("a.drp", "").empty());
  CHECK(texdump_rel("a.drp", "../x").empty());
  CHECK(texdump_rel("a.drp", "sub/x").empty());
  CHECK(texdump_rel("a.drp", "c:x").empty());
  CHECK(texdump_rel("../a.drp", "x").empty());
  CHECK(texdump_rel("/a.drp", "x").empty());
  CHECK(texdump_rel("a.drp", std::string("x\x01y")).empty());
#ifdef _WIN32
  _putenv_s("SNAP_TEXDUMP_DIR", "C:/tmp/tc"); _putenv_s("SNAP_TEXDUMP", "0");
#else
  setenv("SNAP_TEXDUMP_DIR", "C:/tmp/tc", 1); setenv("SNAP_TEXDUMP", "0", 1);
#endif
  CHECK(texdump_root() == "C:/tmp/tc" && !texdump_enabled());
#ifdef _WIN32
  _putenv_s("SNAP_TEXDUMP", "1");
#else
  setenv("SNAP_TEXDUMP", "1", 1);
#endif
  CHECK(texdump_enabled());

  // Index: first (highest priority) pack wins; the game's name may carry its extension
  Index ix;
  ix.packs.resize(2);
  ix.packs[0].id = "hi";
  ix.packs[1].id = "lo";
  ix.add(0, "a.drp", "icon", ext_crc("nutexb"), "C:/hi/icon.nutexb");
  ix.add(1, "a.drp", "icon", ext_crc("nutexb"), "C:/lo/icon.nutexb");
  ix.add(1, "a.drp", "other", ext_crc("msbt"), "C:/lo/other.msbt");
  CHECK(ix.map.size() == 2 && ix.conflicts.size() == 1);
  const Override* o = ix.find("a.drp", "Icon", ext_crc("nutexb"));
  CHECK(o && o->pack == 0 && o->file == "C:/hi/icon.nutexb");
  CHECK(ix.find("a.drp", "Icon.nutexb", ext_crc("nutexb")) == o);          // stored name with extension
  CHECK(!ix.find("a.drp", "Icon", ext_crc("msbt")));                       // same name, other type
  CHECK(!ix.find("b.drp", "icon", ext_crc("nutexb")));                     // other archive
  CHECK(ix.find("a.drp", "OTHER", ext_crc("msbt")) != nullptr);

  // load(): mods.json order and enabled flags, pack.json, ignored files
  fs::path root = fs::temp_directory_path() / "snap_mods_test";
  fs::remove_all(root);
  write(root / "packA" / "pack.json", "{\"name\":\"Pack A\",\"version\":\"1.0\",\"author\":\"t\",\"description\":\"d\"}");
  write(root / "packA" / "common/UI/x.drp/icon.nutexb", "AAAA");
  write(root / "packB" / "pack.json", "{\"name\":\"Pack B\"}");
  write(root / "packB" / "common/UI/x.drp/icon.nutexb", "BBBB");
  write(root / "packB" / "common/UI/x.drp/menu.msbt", "MENU");
  write(root / "packB" / "readme.txt", "ignored");
  write(root / "packC" / "pack.json", "{}");
  write(root / "packC" / "common/UI/x.drp/off.msbt", "OFF");
  write(root / "packA" / "romfs/common/UI/x.drp", "WHOLE");     // whole-file layer: not an entry override
  write(root / "packC" / "romfs/common/UI/y.drp", "OFF");       // disabled pack: no layer
  write(root / "mods.json", "{\"packs\":[{\"id\":\"packB\"},{\"id\":\"packA\",\"enabled\":true},{\"id\":\"packC\",\"enabled\":false},{\"id\":\"missing\"}]}");
  {
    Index i2;
    std::vector<std::string> w;
    load(root.string(), i2, w);
    CHECK(i2.packs.size() == 3 && i2.packs[0].name == "Pack B" && i2.packs[1].version == "1.0");
    CHECK(i2.map.size() == 2);   // icon (B wins), menu; packC disabled
    const Override* e = i2.find("common/ui/x.drp", "icon", ext_crc("nutexb"));
    CHECK(e && i2.packs[e->pack].id == "packB");
    CHECK(!i2.find("common/ui/x.drp", "off", ext_crc("msbt")));
    CHECK(w.size() == 1);        // "missing" folder
    CHECK(i2.rom_layers.size() == 1 && i2.packs[i2.rom_layers[0].second].id == "packA");
    CHECK(fs::path(i2.rom_layers[0].first) == root / "packA" / "romfs");
    CHECK(i2.conflicts.size() == 1 && i2.conflicts[0].path == "common/UI/x.drp/icon.nutexb");   // the file as named in the pack
    CHECK(i2.packs[i2.conflicts[0].winner].id == "packB" && i2.packs[i2.conflicts[0].loser].id == "packA");
  }

  // Zipped packs: the pack inside one top folder of the zip; it outranks packB; packA and packZ both carry a whole file
  write_zip(root / "packZ.zip", {{"packZ/pack.json", "{\"name\":\"Zip pack\",\"version\":\"2\"}"},
                                 {"packZ/common/UI/x.drp/icon.nutexb", "ZZZZ"},
                                 {"packZ/common/UI/x.drp/zip.msbt", "ZIP"},
                                 {"packZ/romfs/common/UI/x.drp", "ZWHOLE"}});
  write_zip(root / "evil.zip", {{"pack.json", "{}"}, {"../outside.txt", "nope"}});
  write_zip(root / "quiet.zip", {{"pack.json", "{\"name\":\"Quiet\"}"}, {"common/UI/x.drp/q.msbt", "Q"}});
  {
    auto ids = packs_on_disk(root.string());
    CHECK(ids == std::vector<std::string>({"evil", "packA", "packB", "packC", "packZ", "quiet"}));
    Pack q;
    std::vector<std::string> w;
    CHECK(pack_info(root.string(), "quiet", q, w) && q.zip && q.name == "Quiet");   // read from the zip, not unpacked
    CHECK(!fs::exists(root / ".unpacked" / "quiet"));
    CHECK(!pack_info(root.string(), "nothing", q, w));
  }
  write(root / "mods.json", "{\"packs\":[\"packZ\",\"packB\",\"packA\",\"evil\",{\"id\":\"quiet\",\"enabled\":false}]}");
  {
    // the launcher's view: same conflicts, nothing unpacked
    Index d;
    std::vector<std::string> w;
    load(root.string(), d, w, false);
    CHECK(!fs::exists(root / ".unpacked" / "packZ") && !fs::exists(root / ".unpacked" / "evil"));
    CHECK(d.packs.size() == 5 && d.packs[0].zip && d.packs[0].enabled && !d.packs[3].enabled);
    const Override* e = d.find("common/ui/x.drp", "icon", ext_crc("nutexb"));
    CHECK(e && d.packs[e->pack].id == "packZ" && e->file.empty());
    int icon = 0, whole = 0;
    for (auto& c : d.conflicts) {
      if (c.path == "common/UI/x.drp/icon.nutexb" && d.packs[c.winner].id == "packZ") icon++;
      if (c.path == "romfs/common/UI/x.drp" && d.packs[c.winner].id == "packZ" && d.packs[c.loser].id == "packA") whole++;
    }
    CHECK(d.conflicts.size() == 3 && icon == 2 && whole == 1);
  }
  {
    Index i5;
    std::vector<std::string> w;
    load(root.string(), i5, w);
    CHECK(i5.packs.size() == 5 && i5.packs[0].zip && i5.packs[0].name == "Zip pack" && i5.packs[0].version == "2");
    const Override* e = i5.find("common/ui/x.drp", "icon", ext_crc("nutexb"));
    CHECK(e && i5.packs[e->pack].id == "packZ" && slurp(e->file) == "ZZZZ");
    CHECK(fs::path(e->file).parent_path().parent_path().parent_path().parent_path() == root / ".unpacked" / "packZ");
    CHECK(i5.find("common/ui/x.drp", "zip", ext_crc("msbt")) != nullptr);
    CHECK(!i5.find("common/ui/x.drp", "q", ext_crc("msbt")));                  // disabled zip: not unpacked, no entries
    CHECK(!fs::exists(root / ".unpacked" / "quiet"));
    CHECK(i5.packs[3].id == "evil" && !i5.packs[3].enabled && !fs::exists(root / "outside.txt") && !fs::exists(root / ".unpacked" / "evil"));
    bool warned = false;
    for (auto& s : w) warned |= s.find("evil.zip: refused") == 0;
    CHECK(warned);
    // conflicts: icon (Z beats B, Z beats A), and the whole file x.drp (Z beats A)
    int icon = 0, whole = 0;
    for (auto& c : i5.conflicts) {
      if (c.path == "common/UI/x.drp/icon.nutexb" && i5.packs[c.winner].id == "packZ") icon++;
      if (c.path == "romfs/common/UI/x.drp" && i5.packs[c.winner].id == "packZ" && i5.packs[c.loser].id == "packA") whole++;
    }
    CHECK(i5.conflicts.size() == 3 && icon == 2 && whole == 1);
  }
  {
    // the unpacked copy is reused while the zip is unchanged, and redone when it changes
    fs::path f = root / ".unpacked" / "packZ" / "common/UI/x.drp/zip.msbt";
    write(f, "LOCAL");
    Index i6;
    std::vector<std::string> w;
    load(root.string(), i6, w);
    CHECK(slurp(f) == "LOCAL");
    write_zip(root / "packZ.zip", {{"pack.json", "{}"}, {"common/UI/x.drp/zip.msbt", "NEW!"}});
    fs::last_write_time(root / "packZ.zip", fs::last_write_time(root / "packZ.zip") + std::chrono::seconds(5));
    Index i7;
    load(root.string(), i7, w);
    CHECK(slurp(f) == "NEW!" && !fs::exists(root / ".unpacked" / "packZ" / "romfs"));
  }
  fs::remove(root / "packZ.zip");
  fs::remove(root / "evil.zip");
  fs::remove(root / "quiet.zip");
  fs::remove_all(root / ".unpacked");
  fs::remove(root / "mods.json");
  {
    Index i3;
    std::vector<std::string> w;
    load(root.string(), i3, w);   // no mods.json: packs are opt-in, except native-options
    CHECK(!i3.find("common/ui/x.drp", "icon", ext_crc("nutexb")));
    CHECK(i3.packs.empty() && i3.map.empty() && i3.rom_layers.empty());
  }
  write(root / "native-options" / "pack.json", "{}");
  write(root / "native-options" / "common/UI/x.drp/pc.msbt", "PC");
  {
    Index i4;
    std::vector<std::string> w;
    load(root.string(), i4, w);
    const Override* e = i4.find("common/ui/x.drp", "pc", ext_crc("msbt"));
    CHECK(e && i4.packs[e->pack].id == "native-options" && i4.map.size() == 1);
  }
  fs::remove_all(root);

  // Size override: the replacement chunk must pass the loader's raw-entry path (csize == dsize), its CRC check, and
  // may be larger than the original. Name "Icon" -> header 0x18 + 8 bytes.
  CHECK(chunk_header_len(4) == 0x18 + 8);
  CHECK(chunk_header_len(3) == 0x18 + 4);
  CHECK(chunk_header_len(7) == 0x18 + 8 && chunk_header_len(8) == 0x18 + 12);
  uint8_t orig[0x18 + 8 + 16] = {};
  uint32_t w0[6] = {0x1111u, 0x64736572u, 0x40u, ext_crc("nutexb"), 16u + 4u, 100u};   // an Oodle original: csize 20, dsize 100
  std::memcpy(orig, w0, sizeof w0);
  std::memcpy(orig + 0x18, "Icon", 5);
  std::vector<uint8_t> big(400);
  for (size_t i = 0; i < big.size(); i++) big[i] = (uint8_t)(i * 7 + 3);   // 4x the original's 100 bytes
  auto ch = build_chunk(orig, chunk_header_len(4), big.data(), big.size());
  uint32_t crc, magic, total, ecrc, csz, dsz;
  std::memcpy(&crc, &ch[0], 4); std::memcpy(&magic, &ch[4], 4); std::memcpy(&total, &ch[8], 4);
  std::memcpy(&ecrc, &ch[12], 4); std::memcpy(&csz, &ch[16], 4); std::memcpy(&dsz, &ch[20], 4);
  CHECK(magic == 0x64736572u && ecrc == ext_crc("nutexb"));
  CHECK(csz == 400 && dsz == 400);                               // raw path: memcpy(dst, payload, csize), dst sized dsize
  CHECK(total == ch.size() && ch.size() == 0x18 + 8 + 400);
  CHECK(crc == crc32(big.data(), big.size()));                   // the loader's check over dsize bytes
  CHECK(std::memcmp(&ch[0x18], "Icon", 5) == 0);                 // name kept
  size_t doff = 0x18 + ((std::strlen((const char*)&ch[0x18]) + 4) & ~(size_t)3);   // how the loader finds the payload
  CHECK(doff == chunk_header_len(4) && std::memcmp(&ch[doff], big.data(), big.size()) == 0);

  // Pack ids from a hostile mods.json: ".." and path-like ids are refused, nothing is read or removed outside the mods folder.
  {
    fs::path r2 = fs::temp_directory_path() / ("mods_ids_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(r2);
    write(r2 / "root" / "mods.json", "{\"packs\":[\"..\",\"a\\\\..\\\\b\",\"C:\\\\x\",\"a/b\",\".\",{\"id\":\"\"}]}");
    write(r2 / "pack.json", "{\"name\":\"Parent\"}");              // what ".." would reach
    write(r2 / "root" / "b" / "pack.json", "{}");                   // what "a\..\b" would reach
    Index ix;
    std::vector<std::string> w;
    load((r2 / "root").string(), ix, w);
    CHECK(ix.packs.empty());
    CHECK(w.size() >= 4);
    Pack q;
    std::vector<std::string> w2;
    const std::string rt = (r2 / "root").string();
    CHECK(!pack_info(rt, "..", q, w2));
    CHECK(!pack_info(rt, "a\\..\\b", q, w2));
    CHECK(!pack_info(rt, "C:\\x", q, w2));
    CHECK(unpack_zip(rt, "..", w2).empty());
    CHECK(unpack_zip(rt, "a\\..\\b", w2).empty());
    CHECK(unpack_zip(rt, "C:\\x", w2).empty());
    CHECK(fs::exists(r2 / "pack.json") && fs::exists(r2 / "root" / "b" / "pack.json"));
    fs::remove_all(r2);
  }

  // Review fix: Windows strips trailing dots and spaces, so ".. " named the mods root and unpack_zip's remove_all wiped every
  // pack. Trailing dots / spaces and DOS device names are refused; plain names (dots and spaces inside) still pass.
  {
    for (const char* bad : {".. ", ". ", "a.", "a ", "..  ", ". .", "con", "CON", "nul", "Nul.txt", "aux", "prn", "com1", "LPT9",
                            "nul .zip", "conin$", "a\tb", "a*b", "a?b", "a<b", "a|b"})
      if (safe_pack_id(bad)) { std::printf("  safe_pack_id accepted \"%s\"\n", bad); CHECK(false); }
    for (const char* good : {"a", "My Pack", "pack.v2", "a.b c", "console", "nullify", "com10", "lpt", ".hidden"})
      if (!safe_pack_id(good)) { std::printf("  safe_pack_id refused \"%s\"\n", good); CHECK(false); }
    fs::path r4 = fs::temp_directory_path() / ("mods_dots_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(r4);
    write(r4 / "root" / "keep" / "pack.json", "{}");
    write(r4 / "root" / ".unpacked" / "other" / "x.txt", "x");
    write(r4 / "root" / "mods.json", "{\"packs\":[\".. \",\". \",\"a.\",\"con\"]}");
    Index ix;
    std::vector<std::string> w;
    load((r4 / "root").string(), ix, w);
    CHECK(ix.packs.empty() || (ix.packs.size() == 1 && ix.packs[0].id == "keep"));
    const std::string rt = (r4 / "root").string();
    CHECK(unpack_zip(rt, ".. ", w).empty());
    CHECK(unpack_zip(rt, ". ", w).empty());
    CHECK(fs::exists(r4 / "root" / "keep" / "pack.json") && fs::exists(r4 / "root" / ".unpacked" / "other" / "x.txt"));
    fs::remove_all(r4);
  }

  // A deflate bomb: a tiny stream that expands far past the size the zip declares. extract() must refuse it and stop writing at that size.
  {
    fs::path r3 = fs::temp_directory_path() / ("mods_bomb_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(r3);
    fs::create_directories(r3);
    std::string d;                       // one fixed-Huffman block: literal 'a', then 20000 x (length 258, distance 1), then end of block
    uint32_t acc = 0; int nb = 0;
    auto bits = [&](uint32_t v, int n) { for (int i = 0; i < n; ++i) { acc |= ((v >> i) & 1u) << nb; if (++nb == 8) { d += (char)acc; acc = 0; nb = 0; } } };
    auto code = [&](uint32_t v, int n) { for (int i = n - 1; i >= 0; --i) bits((v >> i) & 1u, 1); };   // Huffman codes go out MSB first
    bits(1, 1); bits(1, 2);
    code(0x30 + 'a', 8);
    for (int i = 0; i < 20000; ++i) { code(0xC0 + (285 - 280), 8); code(0, 5); }
    code(0, 7);
    if (nb) d += (char)acc;
    const uint32_t declared = 100;
    std::string z, cd;
    auto p16 = [](std::string& s, uint32_t v) { s += (char)(v & 0xff); s += (char)((v >> 8) & 0xff); };
    auto p32 = [&](std::string& s, uint32_t v) { p16(s, v & 0xffff); p16(s, v >> 16); };
    const std::string nm = "bomb.bin";
    p32(z, 0x04034b50); p16(z, 20); p16(z, 0); p16(z, 8); p16(z, 0); p16(z, 0x21); p32(z, 0x12345678); p32(z, (uint32_t)d.size()); p32(z, declared);
    p16(z, (uint32_t)nm.size()); p16(z, 0); z += nm; z += d;
    p32(cd, 0x02014b50); p16(cd, 20); p16(cd, 20); p16(cd, 0); p16(cd, 8); p16(cd, 0); p16(cd, 0x21); p32(cd, 0x12345678); p32(cd, (uint32_t)d.size()); p32(cd, declared);
    p16(cd, (uint32_t)nm.size()); p16(cd, 0); p16(cd, 0); p16(cd, 0); p16(cd, 0); p32(cd, 0); p32(cd, 0); cd += nm;
    uint32_t cdo = (uint32_t)z.size();
    z += cd;
    p32(z, 0x06054b50); p16(z, 0); p16(z, 0); p16(z, 1); p16(z, 1); p32(z, (uint32_t)cd.size()); p32(z, cdo); p16(z, 0);
    write(r3 / "bomb.zip", z);
    std::vector<hle::zipr::Entry> ents;
    std::string err;
    CHECK(hle::zipr::list(r3 / "bomb.zip", ents, &err) && ents.size() == 1);
    CHECK(!hle::zipr::extract(r3 / "bomb.zip", ents[0], r3 / "out.bin", &err));
    std::error_code fec;
    CHECK(fs::file_size(r3 / "out.bin", fec) <= declared);           // the 5 MB of output was never written
    fs::remove_all(r3);
  }

  printf("%d checks, %d failed\n", g_n, g_fail);
  return g_fail ? 1 : 0;
}
