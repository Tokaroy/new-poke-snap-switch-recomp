// SPDX-License-Identifier: MIT
// See elf_check.h. The structure mirrors loader.cpp's map_module: every offset it reads is checked here first.
#include "elf_check.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace elf_check {
namespace {
#pragma pack(push, 1)
struct Ehdr { uint8_t id[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Phdr { uint32_t type, flags; uint64_t off, vaddr, paddr, filesz, memsz, align; };
struct Dyn { int64_t tag; uint64_t val; };
#pragma pack(pop)
constexpr uint64_t kMaxSpan = 1ull << 30;   // the largest guest image the loader will map (the real ones are tens of MB)
}  // namespace

bool check_bytes(const uint8_t* d, size_t n, std::string& err) {
  auto fail = [&](const std::string& m) { err = m; return false; };
  if (n == 0) return fail("is empty");
  if (n < sizeof(Ehdr)) return fail("is too short to be a game file (" + std::to_string(n) + " bytes); it looks cut off");
  Ehdr eh;
  std::memcpy(&eh, d, sizeof eh);
  if (std::memcmp(eh.id, "\x7f" "ELF", 4) != 0) return fail("is not a converted game file (it does not start with the ELF marker)");
  if (eh.id[4] != 2 || eh.id[5] != 1 || eh.machine != 0xB7) return fail("is not a 64-bit ARM game file");
  if (eh.phentsize != sizeof(Phdr) || eh.phnum == 0 || eh.phnum > 64) return fail("has no usable program headers");
  if (eh.phoff > n || (uint64_t)eh.phnum * sizeof(Phdr) > n - eh.phoff) return fail("is cut off: its program headers go past the end");
  std::vector<Phdr> ph(eh.phnum);
  std::memcpy(ph.data(), d + eh.phoff, ph.size() * sizeof(Phdr));

  uint64_t hi = 0, dynva = 0;
  bool have_text = false, have_dyn = false;
  for (const Phdr& p : ph) {
    if (p.type == 2) { dynva = p.vaddr; have_dyn = true; }
    if (p.type != 1) continue;
    if (p.filesz > p.memsz) return fail("is damaged: a segment is bigger in the file than in memory");
    if (p.off > n || p.filesz > n - p.off) {
      const uint64_t need = p.off + p.filesz;
      return fail("is cut off: it has " + std::to_string(n) + " bytes but needs at least " + std::to_string(need));
    }
    if (p.vaddr > kMaxSpan || p.memsz > kMaxSpan || p.vaddr + p.memsz > kMaxSpan) return fail("is damaged: a segment has an impossible size");
    if (p.vaddr + p.memsz > hi) hi = p.vaddr + p.memsz;
    if (p.flags & 1) have_text = true;
  }
  if (hi == 0) return fail("has no code or data to load");
  if (!have_text) return fail("has no code segment");
  if (!have_dyn) return fail("has no dynamic section, so it cannot be linked");
  const uint64_t span = (hi + 0xFFFF) & ~0xFFFFull;

  // the dynamic section lives inside a loaded segment; read its tags from the file bytes
  const Phdr* seg = nullptr;
  for (const Phdr& p : ph) if (p.type == 1 && dynva >= p.vaddr && dynva < p.vaddr + p.filesz) { seg = &p; break; }
  if (!seg) return fail("is damaged: its dynamic section is not inside the file");
  uint64_t off = seg->off + (dynva - seg->vaddr);
  const uint64_t seg_end = seg->off + seg->filesz;
  uint64_t strtab = 0, symtab = 0, hash = 0, rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0;
  bool terminated = false;
  for (; off + sizeof(Dyn) <= seg_end; off += sizeof(Dyn)) {
    Dyn dy;
    std::memcpy(&dy, d + off, sizeof dy);
    if (dy.tag == 0) { terminated = true; break; }
    switch (dy.tag) {
      case 5: strtab = dy.val; break;
      case 6: symtab = dy.val; break;
      case 4: hash = dy.val; break;
      case 7: rela = dy.val; break;
      case 8: relasz = dy.val; break;
      case 23: jmprel = dy.val; break;
      case 2: pltrelsz = dy.val; break;
      default: break;
    }
  }
  if (!terminated) return fail("is damaged: its dynamic section never ends");
  if (!strtab || !symtab) return fail("is damaged: it has no symbol table");
  auto inside = [&](uint64_t a, uint64_t len) { return a < span && len <= span - a; };
  if (!inside(strtab, 1) || !inside(symtab, 24)) return fail("is damaged: its symbol table is outside the program");
  if (hash && !inside(hash, 8)) return fail("is damaged: its symbol hash is outside the program");
  if ((rela || relasz) && !inside(rela, relasz)) return fail("is damaged: its relocation table is outside the program");
  if ((jmprel || pltrelsz) && !inside(jmprel, pltrelsz)) return fail("is damaged: its relocation table is outside the program");
  return true;
}

bool check_file(const char* path, const char* label, std::string& err) {
  const std::string who = std::string("The ") + label + " file";
  FILE* f = std::fopen(path, "rb");
  if (!f) { err = who + " cannot be opened (" + path + ")."; return false; }
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n < 0) { std::fclose(f); err = who + " cannot be read."; return false; }
  std::vector<uint8_t> v((size_t)n);
  const bool ok = n == 0 || std::fread(v.data(), 1, (size_t)n, f) == (size_t)n;
  std::fclose(f);
  if (!ok) { err = who + " cannot be read (the disk or share may have disconnected)."; return false; }
  std::string why;
  if (check_bytes(v.data(), v.size(), why)) return true;
  err = who + " " + why + ". Run the builder again to convert the game.";
  return false;
}

}  // namespace elf_check
