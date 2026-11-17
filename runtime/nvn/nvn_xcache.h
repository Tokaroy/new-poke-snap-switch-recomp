// Translation-cache entry protocol (runtime/nvn/nvn_shader.cpp BuildStage). Header only, no Vulkan: tests/nvn/xcache_test.cpp includes it.
//
// An entry is <key>.spv + <key>.json (the json is published last) and <key>.co, a sidecar holding the constOff the translation was made with.
//
// 1. Writers never share scratch files. Every translation works on <key>.x<pid>_<seq>.* paths, so one writer cannot delete or rename another's
//    input or output (before this, every writer used <key>.bin, <key>.spv.tmp and <key>.json.tmp).
// 2. One writer per key at a time, across threads and across processes: a named mutex per key (Local\, so per Windows session). A writer that
//    missed the cache takes the claim, checks the cache again (another writer may have published while it waited), and only then translates.
//    A writer that dies holds an abandoned mutex: the next claimant gets it (WAIT_ABANDONED), sweeps the dead writer's <key>.x* files and goes on.
// 3. Final files are only removed while holding the claim.
// 4. constOff is a translation input (NvnAccessor reads constant buffer 1 and bounds the code span with it) but the key hashes only the data
//    section, stage and codeOff, and keys double as shader identities (feature tables, recorded pipeline keys). Renaming every shader is not an
//    option, so the key stays and the sidecar records constOff: ResolveKey returns the old key when the sidecar is absent (an older cache entry,
//    trusted) or matches, and a second key (salt + "|co<constOff>") when it differs. This game never hits the second case (census of all 6,227
//    programs: no two share data, stage and codeOff with different constOff), so every existing key and cache entry stays valid.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace nvn::xcache {

// ".x<pid>_<seq>": private to this process and request.
inline std::string UniqueSuffix() {
  static std::atomic<unsigned> seq{0};
  char b[48];
  snprintf(b, sizeof b, ".x%lu_%u", (unsigned long)GetCurrentProcessId(), ++seq);
  return b;
}

inline bool FileExists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

inline bool ReadCo(const std::string& coPath, uint32_t* v) {
  std::ifstream f(coPath);
  unsigned long long x;
  if (!(f >> x) || x > 0xFFFFFFFFull) return false;
  *v = (uint32_t)x;
  return true;
}

// Writes <path> through a private temp name and a rename, so a reader never sees a half-written file.
inline bool WriteAtomic(const std::string& path, const void* d, size_t n) {
  const std::string tmp = path + UniqueSuffix() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(static_cast<const char*>(d), (std::streamsize)n);
    if (!f.good()) { f.close(); DeleteFileA(tmp.c_str()); return false; }
  }
  if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(tmp.c_str()); return false; }
  return true;
}
inline bool WriteCo(const std::string& coPath, uint32_t constOff) {
  const std::string s = std::to_string(constOff);
  return WriteAtomic(coPath, s.data(), s.size());
}

// The key to store this program under: `hash(extra)` is the hex SHA-256 of the data section followed by `extra`.
template <class Hash>
std::string ResolveKey(const std::string& cacheDir, Hash hash, const std::string& salt, int stage, uint32_t codeOff, uint32_t constOff) {
  const std::string common = salt + "|" + std::to_string(stage) + "|" + std::to_string(codeOff);
  std::string key = hash(common);
  uint32_t have = 0;
  if (!key.empty() && ReadCo(cacheDir + "\\" + key + ".co", &have) && have != constOff) key = hash(common + "|co" + std::to_string(constOff));
  return key;
}

// One writer per key. The wait is bounded (a harness run is capped at 10 minutes); after it the caller carries on without the claim, which is
// still safe because it only touches its own private files and publishes with renames.
class KeyClaim {
 public:
  explicit KeyClaim(const std::string& key, DWORD waitMs = 12 * 60 * 1000) {
    m_ = CreateMutexA(nullptr, FALSE, ("Local\\SnapXlate_" + key).c_str());
    if (!m_) return;
    const DWORD r = WaitForSingleObject(m_, waitMs);
    held_ = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
    abandoned_ = r == WAIT_ABANDONED;
  }
  ~KeyClaim() {
    if (m_) { if (held_) ReleaseMutex(m_); CloseHandle(m_); }
  }
  KeyClaim(const KeyClaim&) = delete;
  KeyClaim& operator=(const KeyClaim&) = delete;
  bool held() const { return held_; }
  bool abandoned() const { return abandoned_; }
  HANDLE leak() { held_ = false; HANDLE h = m_; m_ = nullptr; return h; }   // test aid: keep the claim past the object (a thread that ends holding it abandons it)

 private:
  HANDLE m_ = nullptr;
  bool held_ = false, abandoned_ = false;
};

inline void DeleteMatching(const std::string& pattern, const std::string& dir) {
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((dir + "\\" + pattern).c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileA((dir + "\\" + fd.cFileName).c_str()); } while (FindNextFileA(h, &fd));
  FindClose(h);
}

// Makes sure <dir>\<key>.spv/.json exist and are valid.
//   tryLoad()          -> true when the entry on disk is complete and valid (it must not delete anything)
//   translate(priv)    -> writes priv+".spv" and priv+".json" (any scratch input it needs must also start with priv); returns success
// Returns whether the entry is loadable at the end. `translated` (optional) says whether this call ran the translation.
template <class Load, class Translate>
bool EnsureEntry(const std::string& dir, const std::string& key, uint32_t constOff, Load tryLoad, Translate translate, bool* translated = nullptr) {
  const std::string base = dir + "\\" + key;
  if (translated) *translated = false;
  bool loaded = tryLoad();
  if (!loaded) {
    KeyClaim claim(key);
    loaded = tryLoad();   // another writer may have published while this one waited
    if (!loaded) {
      if (claim.held()) {   // only the claim holder removes final files or another writer's leftovers
        DeleteFileA((base + ".json").c_str()); DeleteFileA((base + ".spv").c_str()); DeleteFileA((base + ".co").c_str());
        DeleteMatching(key + ".x*", dir);
      }
      const std::string priv = base + UniqueSuffix();
      if (translated) *translated = true;
      bool ok = translate(priv);
      if (ok) ok = MoveFileExA((priv + ".spv").c_str(), (base + ".spv").c_str(), MOVEFILE_REPLACE_EXISTING) != 0 && WriteCo(base + ".co", constOff) &&
                   MoveFileExA((priv + ".json").c_str(), (base + ".json").c_str(), MOVEFILE_REPLACE_EXISTING) != 0;   // json last: it completes the entry
      DeleteFileA((priv + ".spv").c_str()); DeleteFileA((priv + ".json").c_str()); DeleteFileA((priv + ".bin").c_str());   // only this request's own files
      if (ok) loaded = tryLoad();
    }
  }
  if (loaded && !FileExists(base + ".co")) WriteCo(base + ".co", constOff);   // an entry from before the sidecar existed: record what it is taken to be
  return loaded;
}

}  // namespace nvn::xcache
