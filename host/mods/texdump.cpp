// SPDX-License-Identifier: MIT
// Texture capture for the builder's AI texture upscaling (project code, ours). Guide: docs/hd-textures-ai.md.
//
// With SNAP_TEXDUMP on (mods_core.h texdump_enabled), every nutexb the DRP entry worker loads is saved once, decompressed,
// at <texdump_root>/<archive>/<entry name>, the same layout a pack folder uses. The builder upscales from that cache, so
// it never has to read DRP archives or decode Oodle itself.
//
// Called from rt_mod_entry (runtime/mods/mods.cpp) at main+0x5e3a58, before the worker decompresses the entry. The chunk
// (x20) is already decrypted. Compressed payloads start with the magic 0xe42d98ba; the worker then calls OodleLZ_Decompress
// (main+0x7240) as (payload+4, csize-4, out, dsize, 1, 0, 0, 0) with five zero stack words and a sixth of 3 (see the
// generated f_71005e38b0). We make the same call into a host buffer on a private host stack, check the result against the
// chunk's CRC32 (the worker's own check) and only then write the file. Nothing the game sees changes.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "loader.h"
#include "mods/mods_core.h"

extern void (*g_snap_texdump)(Cpu* c, const std::string& archive);

namespace {

constexpr uint64_t kOodleDecompress = 0x7100007240ull;   // main+0x7240
constexpr uint32_t kOodleMagic = 0xe42d98bau;
constexpr uint32_t kNutexb = 0x5c156dbcu;
constexpr size_t kStackBytes = 1u << 20;                 // private stack for the nested Oodle call

std::mutex g_mu;
std::unordered_set<std::string> g_seen;                   // rel paths handled this run (saved, present or failed)
std::atomic<unsigned> g_saved{0}, g_present{0}, g_failed{0};
std::string g_root;

std::wstring widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
  return w;
}

bool make_dirs(const std::wstring& dir) {
  for (size_t i = 3; i <= dir.size(); i++) {
    if (i == dir.size() || dir[i] == L'\\') {
      std::wstring part = dir.substr(0, i);
      if (!CreateDirectoryW(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
  }
  return true;
}

// Size of an existing file, or -1.
long long file_size(const std::wstring& p) {
  WIN32_FILE_ATTRIBUTE_DATA a;
  if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return -1;
  return ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

bool write_atomic(const std::wstring& path, const uint8_t* data, size_t n) {
  std::wstring tmp = path + L".part";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  size_t done = 0;
  bool ok = true;
  while (ok && done < n) {
    DWORD put = 0;
    ok = WriteFile(h, data + done, (DWORD)std::min<size_t>(n - done, 1u << 24), &put, nullptr) && put;
    done += put;
  }
  CloseHandle(h);
  if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); return false; }
  return true;
}

void append_index(const std::string& rel, uint32_t dsize, uint32_t crc) {
  std::lock_guard<std::mutex> g(g_mu);
  FILE* f = _wfopen(widen(g_root + "\\index.tsv").c_str(), L"ab");
  if (!f) return;
  fprintf(f, "%s\t%u\t%08x\t%lld\n", rel.c_str(), dsize, crc, (long long)time(nullptr));
  fclose(f);
}

// The game exits through a path that skips atexit, so progress is printed as it goes.
void progress() {
  fprintf(stderr, "[texdump] %u saved, %u already cached, %u failed (%s)\n", g_saved.load(), g_present.load(), g_failed.load(), g_root.c_str());
}

void dump(Cpu* c, const std::string& archive) {
  const uint8_t* chunk = (const uint8_t*)c->x[20];
  uint32_t w[6];
  std::memcpy(w, chunk, sizeof w);   // crc32 of the output, 'resd', total, ext crc, csize, dsize
  if (w[3] != kNutexb) return;
  const uint32_t crc = w[0], csize = w[4], dsize = w[5];
  if (!dsize || dsize > (512u << 20) || csize > (512u << 20)) return;
  const char* name = (const char*)chunk + 0x18;
  const size_t nlen = strnlen(name, 256);
  const std::string rel = mods::texdump_rel(archive, std::string(name, nlen));
  if (rel.empty()) return;
  {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_seen.insert(rel).second) return;
  }
  std::string relw = rel;
  for (auto& ch : relw) if (ch == '/') ch = '\\';
  const std::wstring path = widen(g_root + "\\" + relw);
  if (file_size(path) == (long long)dsize) { g_present++; return; }

  const uint8_t* payload = chunk + mods::chunk_header_len(nlen);
  std::vector<uint8_t> out(dsize + 64);
  if (csize == dsize) {
    std::memcpy(out.data(), payload, dsize);
  } else {
    uint32_t magic;
    std::memcpy(&magic, payload, 4);
    if (magic != kOodleMagic || csize < 8) { if (g_failed++ < 10) fprintf(stderr, "[texdump] %s: unknown packing %08x\n", rel.c_str(), magic); return; }
    thread_local std::vector<uint8_t> stack;
    if (stack.empty()) stack.resize(kStackBytes);
    const uint64_t saved_sp = c->sp;
    const uint64_t top = ((uint64_t)(uintptr_t)(stack.data() + stack.size()) - 0x100) & ~0xfull;
    uint64_t args[6] = {0, 0, 0, 0, 0, 3};   // decBufBase, decBufSize, fpCallback, callbackUserData, decoderMemory, threadPhase(32-bit)
    std::memcpy((void*)(uintptr_t)top, args, sizeof args);
    c->sp = top;
    const uint64_t got = loader::call_guest_nested(c, kOodleDecompress,
        {(uint64_t)(uintptr_t)(payload + 4), csize - 4, (uint64_t)(uintptr_t)out.data(), dsize, 1, 0, 0, 0});
    c->sp = saved_sp;
    if (got != dsize) { if (g_failed++ < 10) fprintf(stderr, "[texdump] %s: Oodle returned %llu of %u bytes\n", rel.c_str(), (unsigned long long)got, dsize); return; }
  }
  if (mods::crc32(out.data(), dsize) != crc) { if (g_failed++ < 10) fprintf(stderr, "[texdump] %s: CRC mismatch, not saved\n", rel.c_str()); return; }
  const size_t cut = path.find_last_of(L'\\');
  if (cut == std::wstring::npos || !make_dirs(path.substr(0, cut)) || !write_atomic(path, out.data(), dsize)) {
    if (g_failed++ < 10) fprintf(stderr, "[texdump] %s: cannot write\n", rel.c_str());
    return;
  }
  append_index(rel, dsize, crc);
  if (++g_saved % 500 == 0) progress();
}

struct Install {
  Install() {
    if (!mods::texdump_enabled()) return;
    std::wstring r = widen(mods::texdump_root()), full(MAX_PATH * 4, L'\0');
    DWORD n = GetFullPathNameW(r.c_str(), (DWORD)full.size(), full.data(), nullptr);
    if (n && n < full.size()) { full.resize(n); r = full; }
    while (r.size() > 3 && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    int len = WideCharToMultiByte(CP_UTF8, 0, r.c_str(), (int)r.size(), nullptr, 0, nullptr, nullptr);
    g_root.assign(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, r.c_str(), (int)r.size(), g_root.data(), len, nullptr, nullptr);
    if (!make_dirs(r)) { fprintf(stderr, "[texdump] cannot create %s: capture off\n", g_root.c_str()); return; }
    g_snap_texdump = dump;
  }
} g_install;

}  // namespace
