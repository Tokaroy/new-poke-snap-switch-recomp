// SPDX-License-Identifier: MIT
// Unit test for host/elf_check.cpp: empty, truncated and damaged converted game files must be refused with a message, never crash.
// Build and run with tests\hle\run_elf_check_test.ps1. Optional: elf_check_test.exe <main_elf> <sdk_elf> also checks real files.
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../../host/elf_check.h"

namespace {
int g_fail = 0;
void expect(bool ok, const char* what) {
  if (!ok) { std::printf("FAIL: %s\n", what); ++g_fail; }
}

#pragma pack(push, 1)
struct Ehdr { uint8_t id[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Phdr { uint32_t type, flags; uint64_t off, vaddr, paddr, filesz, memsz, align; };
struct Dyn { int64_t tag; uint64_t val; };
#pragma pack(pop)

// A minimal valid image: a code segment, a data segment holding the dynamic section, and the PT_DYNAMIC header.
std::vector<uint8_t> make_valid() {
  std::vector<uint8_t> f(0x700, 0);
  Ehdr eh{};
  std::memcpy(eh.id, "\x7f" "ELF", 4);
  eh.id[4] = 2; eh.id[5] = 1; eh.id[6] = 1;
  eh.type = 3; eh.machine = 0xB7; eh.version = 1;
  eh.phoff = 64; eh.ehsize = 64; eh.phentsize = 56; eh.phnum = 3;
  std::memcpy(f.data(), &eh, sizeof eh);
  Phdr ph[3] = {};
  ph[0] = {1, 5, 0x400, 0x400, 0x400, 0x100, 0x100, 0x1000};      // PT_LOAD r-x
  ph[1] = {1, 6, 0x500, 0x10500, 0x10500, 0x200, 0x300, 0x1000};  // PT_LOAD rw-
  ph[2] = {2, 6, 0x500, 0x10500, 0x10500, 0x100, 0x100, 8};       // PT_DYNAMIC
  std::memcpy(f.data() + 64, ph, sizeof ph);
  const Dyn dyn[] = {{5, 0x10600}, {6, 0x10640}, {4, 0x10680}, {7, 0x10700}, {8, 0x18}, {0, 0}};
  std::memcpy(f.data() + 0x500, dyn, sizeof dyn);
  return f;
}

bool check(const std::vector<uint8_t>& f, std::string* why = nullptr) {
  std::string e;
  const bool ok = elf_check::check_bytes(f.data(), f.size(), e);
  if (why) *why = e;
  if (!ok && e.empty()) { std::printf("FAIL: a refusal came with no message\n"); ++g_fail; }
  return ok;
}

bool write_file(const std::string& path, const std::vector<uint8_t>& f) {
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) return false;
  if (!f.empty()) std::fwrite(f.data(), 1, f.size(), fp);
  std::fclose(fp);
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  const std::vector<uint8_t> good = make_valid();
  expect(check(good), "a well-formed image passes");

  // every truncation of a valid image is refused (this is what a half-copied file looks like)
  for (size_t n = 0; n < good.size(); ++n) {
    std::vector<uint8_t> cut(good.begin(), good.begin() + (long)n);
    if (check(cut)) { std::printf("FAIL: a copy cut to %zu bytes passed\n", n); ++g_fail; }
  }
  std::string why;
  check({}, &why);
  expect(why == "is empty", "empty file says it is empty");
  check(std::vector<uint8_t>(good.begin(), good.begin() + 20), &why);
  expect(why.find("cut off") != std::string::npos, "20 bytes says cut off");
  check(std::vector<uint8_t>(good.begin(), good.begin() + 0x6ff), &why);
  expect(why.find("cut off") != std::string::npos && why.find("1792") != std::string::npos, "short data segment reports the size it needs");

  { auto f = good; f[0] = 'M'; f[1] = 'Z'; expect(!check(f, &why) && why.find("not a converted game file") != std::string::npos, "wrong magic refused"); }
  { auto f = good; f[18] = 0x3E; f[19] = 0; expect(!check(f, &why) && why.find("64-bit ARM") != std::string::npos, "x86-64 ELF refused"); }
  { auto f = good; f[4] = 1; expect(!check(f), "32-bit ELF refused"); }
  { auto f = good; f[56] = 0; f[57] = 0xFF; expect(!check(f), "absurd program header count refused"); }
  { auto f = good; Phdr p; std::memcpy(&p, f.data() + 64 + 56, sizeof p); p.memsz = 1ull << 40; std::memcpy(f.data() + 64 + 56, &p, sizeof p); expect(!check(f), "terabyte segment refused"); }
  { auto f = good; Phdr p; std::memcpy(&p, f.data() + 64 + 56, sizeof p); p.filesz = 0x400; p.memsz = 0x300; std::memcpy(f.data() + 64 + 56, &p, sizeof p); expect(!check(f), "file part bigger than memory part refused"); }
  { auto f = good; Phdr p; std::memcpy(&p, f.data() + 64, sizeof p); p.flags = 4; std::memcpy(f.data() + 64, &p, sizeof p); expect(!check(f, &why) && why.find("no code segment") != std::string::npos, "no executable segment refused"); }
  { auto f = good; Dyn d{0x7ffffff0, 1}; for (int i = 0; i < 5; ++i) std::memcpy(f.data() + 0x500 + i * 16, &d, sizeof d); expect(!check(f), "dynamic section without symbol table refused"); }
  { auto f = good; Dyn d{6, 0x90000000ull}; std::memcpy(f.data() + 0x500 + 16, &d, sizeof d); expect(!check(f, &why) && why.find("outside the program") != std::string::npos, "symbol table past the image refused"); }
  { auto f = good; Dyn d{7, 0x10700}; std::memcpy(f.data() + 0x500 + 3 * 16, &d, sizeof d); Dyn s{8, 1ull << 40}; std::memcpy(f.data() + 0x500 + 4 * 16, &s, sizeof s); expect(!check(f), "huge relocation table refused"); }
  { auto f = good; Dyn d{9, 9}; std::memcpy(f.data() + 0x500 + 5 * 16, &d, sizeof d); for (int i = 6; i < 32; ++i) std::memcpy(f.data() + 0x500 + i * 16, &d, sizeof d); expect(!check(f, &why) && why.find("never ends") != std::string::npos, "unterminated dynamic section refused"); }

  // random damage never crashes the check (it may pass or refuse)
  std::mt19937 rng(12345);
  for (int i = 0; i < 20000; ++i) {
    auto f = good;
    const int flips = 1 + (int)(rng() % 6);
    for (int k = 0; k < flips; ++k) f[rng() % f.size()] = (uint8_t)rng();
    if (rng() % 4 == 0) f.resize(rng() % f.size());
    std::string e;
    elf_check::check_bytes(f.data(), f.size(), e);
  }

  // the file wrapper: missing, empty and cut-off files, as the launcher sees them
  const std::string tmp = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "\\snap_elf_check_test";
  std::string e;
  expect(!elf_check::check_file((tmp + "_missing.bin").c_str(), "main", e) && e.find("cannot be opened") != std::string::npos, "missing file refused");
  expect(write_file(tmp + "_empty.bin", {}), "write empty fixture");
  expect(!elf_check::check_file((tmp + "_empty.bin").c_str(), "sdk", e) && e == "The sdk file is empty. Run the builder again to convert the game.", "empty file message");
  expect(write_file(tmp + "_cut.bin", std::vector<uint8_t>(good.begin(), good.begin() + 0x300)), "write cut fixture");
  expect(!elf_check::check_file((tmp + "_cut.bin").c_str(), "main", e) && e.rfind("The main file is cut off", 0) == 0, "cut file message");
  expect(write_file(tmp + "_good.bin", good), "write good fixture");
  expect(elf_check::check_file((tmp + "_good.bin").c_str(), "main", e), "good file passes");
  for (const char* s : {"_empty.bin", "_cut.bin", "_good.bin"}) std::remove((tmp + s).c_str());

  for (int i = 1; i + 1 < argc; i += 2) {   // real files given on the command line must pass
    std::string r;
    const bool ok = elf_check::check_file(argv[i], i == 1 ? "main" : "sdk", r);
    std::printf("%s: %s\n", argv[i], ok ? "ok" : r.c_str());
    if (!ok) ++g_fail;
  }

  std::printf(g_fail ? "elf_check_test: %d FAILED\n" : "elf_check_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
