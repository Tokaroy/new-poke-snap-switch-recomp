// SPDX-License-Identifier: MIT
// NVN program objects: GLSLC program binaries -> SPIR-V -> VkShaderModule + descriptor layouts.
//
// Translation path (docs/nvn-vulkan-design.md, "Shader pipeline"): the game's shaders are Maxwell SASS inside the
// GLSLC binary (the NVN driver never sees GLSL at run time). The SASS is translated by the Ryujinx shader
// translator (MIT), driven by scripts/shader_harness (dotnet) in --nvn mode, and the result is cached on disk keyed
// by SHA-256 of the program's data section. Nothing of the game is stored in the repository: the cache lives under
// the layer's cache directory (NVN_CACHE_DIR, default <exe dir>\nvn_cache).
//
// Control/data section facts used here come from analysis/shaders/nushdb_layout.md (derived from the game's own
// files): control+0x6F8 program size, +0x700 constant-data offset, +0x704 data size, +0x708 header size (0x30);
// the data section starts with 0x12345678 and, for graphics stages, the Maxwell SPH at +0x30. Compute programs
// have neither.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <sstream>
#include <mutex>
#include <unordered_set>

#include "nvn_aa.h"
#include "nvn_foliage.h"
#include "nvn_hostbuf.h"
#include "nvn_internal.h"
#include "nvn_pipeline.h"
#include "nvn_pom.h"
#include "nvn_rt.h"   // [rt]
#include "nvn_xcache.h"
#include "nvn_xlate_server.h"
#include "os/perf.h"   // SNAP_PERF instrumentation

namespace nvn {
namespace {

// SNAP_XLATE_REV=<n> switches to translator revision n (patched Ryujinx translator, scripts/xlate_patch.sh): the revision joins the cache salt, so
// old and new outputs never share a key, and the nvn_cache and shadercache folders get a _r<n> suffix (nvn_vk.cpp, nvn_pipeline.cpp).
// Unset or 0 = the original translation, unchanged.
const std::string& TranslatorSaltStr() {
  static const std::string s = [] {
    std::string b = "nvn-xlate-v2|binding-model-1|interlock-off|clip7";
    const char* r = getenv("SNAP_XLATE_REV");
    if (r && *r && strcmp(r, "0")) b += std::string("|xlate-r") + r;
    return b;
  }();
  return s;
}

// Shader modules whose SPIR-V uses subgroup operations (GroupNonUniform*, SubgroupLocalInvocationId and the lane masks): their lane math
// assumes 32 lanes, so the pipeline builders pin them to a required subgroup size of 32 (nvn_draw.cpp, g_sgPinStages).
std::mutex g_sgMu;
std::unordered_set<uint64_t> g_sgMods;
bool SpvUsesSubgroup(const std::vector<uint32_t>& w) {
  for (size_t i = 5; i < w.size();) {
    const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFF;
    if (!wc || i + wc > w.size()) return false;
    if (op >= 333 && op <= 366) return true;   // OpGroupNonUniform*
    if (op == 71 && wc >= 4 && w[i + 2] == 11 && (w[i + 3] == 41 || (w[i + 3] >= 4416 && w[i + 3] <= 4420))) return true;   // BuiltIn SubgroupLocalInvocationId, SubgroupEq/Ge/Gt/Le/LtMask
    i += wc;
  }
  return false;
}
void NoteSubgroup(VkShaderModule m, const std::vector<uint32_t>& w) {
  if (!SpvUsesSubgroup(w)) return;
  std::lock_guard<std::mutex> l(g_sgMu);
  g_sgMods.insert((uint64_t)(uintptr_t)m);
}

// ---- SHA-256 (BCrypt) ---------------------------------------------------------------------------------------------
// Load pass (2026-11-13): the provider is opened once (BCrypt algorithm handles are shareable between threads); opening it per
// shader cost about as much as the hash. SNAP_SHADER_FASTIO=0 restores one open per call.
bool ShaderFastIo() {
  static const bool on = [] { const char* e = getenv("SNAP_SHADER_FASTIO"); return !(e && !strcmp(e, "0")); }();
  return on;
}
std::string Sha256Hex(const uint8_t* d, size_t n, const std::string& extra) {
  static BCRYPT_ALG_HANDLE shared = [] {
    BCRYPT_ALG_HANDLE a = nullptr;
    return BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ? nullptr : a;
  }();
  const bool keep = ShaderFastIo() && shared;
  BCRYPT_ALG_HANDLE alg = keep ? shared : nullptr;
  BCRYPT_HASH_HANDLE h = nullptr;
  std::string out;
  if (!keep && BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return out;
  if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) >= 0) {
    BCryptHashData(h, const_cast<PUCHAR>(d), (ULONG)n, 0);
    BCryptHashData(h, (PUCHAR)extra.data(), (ULONG)extra.size(), 0);
    uint8_t dig[32] = {};
    BCryptFinishHash(h, dig, 32, 0);
    char hx[65];
    for (int i = 0; i < 32; i++) snprintf(hx + 2 * i, 3, "%02x", dig[i]);
    out.assign(hx, 64);
    BCryptDestroyHash(h);
  }
  if (!keep) BCryptCloseAlgorithmProvider(alg, 0);
  return out;
}

// ---- tiny JSON reader (the manifest is machine-written, numbers only) ---------------------------------------------
struct J {
  char type = 0;  // 'n' number, 'a' array, 'o' object
  double num = 0;
  std::vector<J> arr;
  std::vector<std::pair<std::string, J>> obj;
  const J* get(const char* k) const {
    for (auto& kv : obj) if (kv.first == k) return &kv.second;
    return nullptr;
  }
  int i(const char* k, int def = 0) const { auto* v = get(k); return v ? (int)v->num : def; }
};
struct JP {
  const char* p; const char* e;
  void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
  bool parse(J& out) {
    ws();
    if (p >= e) return false;
    if (*p == '{') {
      p++; out.type = 'o';
      for (;;) {
        ws();
        if (p < e && *p == '}') { p++; return true; }
        if (p < e && *p == ',') { p++; continue; }
        if (p >= e || *p != '"') return false;
        const char* s = ++p;
        while (p < e && *p != '"') p++;
        std::string k(s, p); p++;
        ws();
        if (p >= e || *p != ':') return false;
        p++;
        J v;
        if (!parse(v)) return false;
        out.obj.emplace_back(k, std::move(v));
      }
    }
    if (*p == '[') {
      p++; out.type = 'a';
      for (;;) {
        ws();
        if (p < e && *p == ']') { p++; return true; }
        if (p < e && *p == ',') { p++; continue; }
        J v;
        if (!parse(v)) return false;
        out.arr.push_back(std::move(v));
      }
    }
    char* end = nullptr;
    out.type = 'n';
    out.num = strtod(p, &end);
    if (end == p) return false;
    p = end;
    return true;
  }
};

bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
  if (ShaderFastIo()) {   // load pass: one sized Win32 read instead of a byte-wise istreambuf_iterator copy (same bytes)
    HANDLE fh = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(fh, &sz) && sz.QuadPart >= 0 && sz.QuadPart < (1ll << 31);
    if (ok) {
      out->resize((size_t)sz.QuadPart);
      DWORD got = 0;
      ok = sz.QuadPart == 0 || (::ReadFile(fh, out->data(), (DWORD)sz.QuadPart, &got, nullptr) && got == (DWORD)sz.QuadPart);
    }
    CloseHandle(fh);
    if (!ok) out->clear();
    return ok;
  }
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}
bool WriteFile(const std::string& path, const void* d, size_t n) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(static_cast<const char*>(d), (std::streamsize)n);
  return f.good();
}

// Translator processes that may run at once (SNAP_XLATE_WORKERS, --shader-workers). 1 = the old way: one process, and a program whose shader
// is not in the cache is translated on the thread that creates it. More than 1: such programs are built in the background by this many
// workers (ProgramSetShadersImpl), each with its own process. auto (default) = half the logical processors, at most 8.
// Measured 2026-11-23: a warm --nvn-serve process takes 6 ms a shader alone and 7 ms (median) while the game boots; 1,647 of them one after
// another held the first launch to the title at 25.6 s against 9.0 s with 8 workers (docs/boot-time-vs-rival.md).
int XlateWorkers() {
  static const int n = [] {
    const char* e = getenv("SNAP_XLATE_WORKERS");
    const int v = (e && *e) ? atoi(e) : 0;   // "auto", empty or unreadable = automatic
    if (v > 0) return std::min(v, 64);
    const int cores = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return std::max(1, std::min(cores / 2, 8));
  }();
  return n;
}

// Never destroyed: background workers may still be inside a request at process exit (the job object ends the processes).
xlate::Pool& ServerPoolFor(const std::string& launch) {
  static std::mutex& mu = *new std::mutex;
  static auto& pools = *new std::map<std::string, std::unique_ptr<xlate::Pool>>;
  std::lock_guard<std::mutex> lk(mu);
  auto& p = pools[launch];
  if (!p) p = std::make_unique<xlate::Pool>(launch + " --nvn-serve", XlateWorkers());
  return *p;
}

// Run the dotnet shader harness (BelowNormal priority, 10 minute cap). Returns true when the manifest exists.
bool RunHarness(const std::string& bin, const std::string& spv, const std::string& json, int stage, uint32_t constOff,
                uint32_t codeOff) {
  std::string dll;
#ifdef NVN_HARNESS_DLL
  dll = NVN_HARNESS_DLL;
#endif
  if (const char* e = getenv("SNAP_XLATE_REV"); e && *e && strcmp(e, "0")) {   // revision n: build\xlate\r<n>\h (scripts/xlate_build.ps1), under the same project root as the default harness
    size_t k = dll.find("tools\\shader_harness");
    if (k == std::string::npos) k = dll.find("tools/shader_harness");
    if (k != std::string::npos) dll = dll.substr(0, k) + "build\\xlate\\r" + e + "\\h\\bin\\Release\\net10.0\\ShaderHarness.dll";
  }
  if (dll.empty()) {   // release build (no baked dev path): the harness folder next to the exe
    char mp[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, mp, MAX_PATH);
    std::string d = mp;
    size_t k = d.find_last_of("\\/");
    if (k != std::string::npos) dll = d.substr(0, k) + "\\shader_harness\\ShaderHarness.exe";
  }
  if (const char* e = getenv("NVN_SHADER_HARNESS")) dll = e;
  if (dll.empty()) { LogError("shader translation needs the harness: set NVN_SHADER_HARNESS=<ShaderHarness.dll>"); return false; }
  const bool native = dll.size() > 4 && _stricmp(dll.c_str() + dll.size() - 4, ".exe") == 0;   // self-contained publish (release): run the apphost, no dotnet
  const std::string launch = std::string(native ? "" : "dotnet ") + "\"" + dll + "\"";

  // One long-lived translator process answers every request (nvn_xlate_server.h): 15 ms a shader against 1.3 s for a process each. SNAP_XLATE_SERVE=0 keeps
  // the one-process-per-shader path; a harness without --nvn-serve, or any trouble with the process, falls through to it as well.
  static const bool serveOn = [] { const char* e = getenv("SNAP_XLATE_SERVE"); return !(e && *e == '0'); }();
  if (serveOn) {
    // Up to XlateWorkers() processes per launch line (a SNAP_XLATE_REV switch never reuses another revision's process); a caller borrows an
    // idle one, starts a new one while under the limit, or waits for one. One process answers one request at a time, as before.
    std::string msg;
    switch (ServerPoolFor(launch).Translate(bin, spv, json, stage, constOff, codeOff, &msg)) {
      case xlate::Server::Result::Ok: return GetFileAttributesA(json.c_str()) != INVALID_FILE_ATTRIBUTES;
      case xlate::Server::Result::Failed: LogError("shader harness failed: %s", msg.c_str()); return false;
      case xlate::Server::Result::Unavailable: break;
    }
  }

  std::ostringstream cl;
  cl << launch << " --nvn \"" << bin << "\" \"" << spv << "\" \"" << json << "\" " << stage << " " << constOff << " " << codeOff;
  std::string cmd = cl.str();
  STARTUPINFOA si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, BELOW_NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    LogError("cannot start dotnet (%lu)", GetLastError());
    return false;
  }
  DWORD w = WaitForSingleObject(pi.hProcess, 600000);
  DWORD code = 1;
  if (w == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
  else TerminateProcess(pi.hProcess, 1);   // our own child only
  CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  if (code != 0) { LogError("shader harness failed (exit %lu)", code); return false; }
  return GetFileAttributesA(json.c_str()) != INVALID_FILE_ATTRIBUTES;
}

VkShaderStageFlagBits StageBit(int nvnStage) {
  static const VkShaderStageFlagBits t[6] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
                                             VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                             VK_SHADER_STAGE_COMPUTE_BIT};
  return t[nvnStage];
}

uint32_t Rd32(const void* p, size_t o) { uint32_t v; memcpy(&v, static_cast<const uint8_t*>(p) + o, 4); return v; }

void FreeStage(Host* h, StageProg& s) {
  if (s.mod) vkDestroyShaderModule(h->device, s.mod, nullptr);
  // fragMods variants are owned by the host (Host::variantMods): composite programs hold copies of this map
  if (s.constBuf) vkDestroyBuffer(h->device, s.constBuf, nullptr);
  if (s.constMem) vkFreeMemory(h->device, s.constMem, nullptr);
  s = StageProg{};
}

// SNAP_XLATE_AB test aid (see XlateAbVariant in nvn_internal.h): loads the same fragment program translated by the other revision
// (SNAP_XLATE_AB_REV, cache dir SNAP_XLATE_AB_DIR) as StageProg::modAlt/spvAlt, with the same post-processing as the normal module.
void LoadAbAlt(Host* h, const uint8_t* data, uint32_t dataSize, int stage, uint32_t codeOff, uint32_t constOff, bool eft, const std::string& baseKey, StageProg* sp) {
  const char* dir = getenv("SNAP_XLATE_AB_DIR");
  if (!dir || !*dir) return;
  const char* r = getenv("SNAP_XLATE_AB_REV");
  std::string salt = "nvn-xlate-v2|binding-model-1|interlock-off|clip7";
  if (r && *r && strcmp(r, "0")) salt += std::string("|xlate-r") + r;
  const std::string key = xcache::ResolveKey(dir, [&](const std::string& extra) { return Sha256Hex(data, dataSize, extra); }, salt, stage, codeOff, constOff);   // same identity rule as BuildStage
  std::vector<uint8_t> spv;
  if (key.empty() || !ReadFile(std::string(dir) + "\\" + key + ".spv", &spv)) { Log("xlate A/B: no alternate translation for %s", baseKey.c_str()); return; }
  std::vector<uint32_t> words(spv.size() / 4);
  memcpy(words.data(), spv.data(), words.size() * 4);
  if (eft) SpvAddEarlyFragmentTests(words);
  SpvAddStorageImageCaps(words);
  SpvSaltForTest(words);
  AaPatchFragment(words, baseKey.c_str());
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = words.size() * 4; ci.pCode = words.data();
  if (vkCreateShaderModule(h->device, &ci, nullptr, &sp->modAlt) != VK_SUCCESS) { sp->modAlt = VK_NULL_HANDLE; return; }
  NoteSubgroup(sp->modAlt, words);
  sp->spvAlt = std::move(words);
}

// In-memory copy of the translation-cache files (load pass 2026-11-21). The game creates about 6,200 programs at boot from about 1,650 distinct
// shaders, and every call read <key>.co, <key>.spv, <key>.json and checked <key>.co again, about 25,000 small file operations on the
// ResourceUncompress thread that holds the DRP lock, which is the critical path of boot. Entries read once are kept here (the cache is not
// changed behind the process's back). SNAP_XCACHE_MEMO_MB=<n> caps the copy (default 256; 0 = read every file as before).
struct XMemoEntry { std::vector<uint8_t> spv, js; };
// Heap objects that are never destroyed: the prefetch thread below may still run at process exit.
std::mutex& g_xmMu = *new std::mutex;
auto& g_xm = *new std::unordered_map<std::string, std::shared_ptr<const XMemoEntry>>;
auto& g_xmCo = *new std::unordered_map<std::string, uint32_t>;   // key -> the constOff its .co sidecar holds (read, or written by this process)
uint64_t g_xmBytes = 0;
uint64_t XMemoCapBytes() {
  static const uint64_t cap = [] {
    const char* e = getenv("SNAP_XCACHE_MEMO_MB");
    const long long mb = (e && *e) ? atoll(e) : 256;
    return mb > 0 ? (uint64_t)mb << 20 : 0ull;
  }();
  return cap;
}
bool XMemoCo(const std::string& dir, const std::string& key, uint32_t* have) {
  if (XMemoCapBytes()) {
    std::lock_guard<std::mutex> l(g_xmMu);
    auto it = g_xmCo.find(key);
    if (it != g_xmCo.end()) { *have = it->second; return true; }
  }
  return xcache::ReadCo(dir + "\\" + key + ".co", have);
}
void XMemoSetCo(const std::string& key, uint32_t v) {
  if (!XMemoCapBytes()) return;
  std::lock_guard<std::mutex> l(g_xmMu);
  g_xmCo[key] = v;
}
std::shared_ptr<const XMemoEntry> XMemoFind(const std::string& key) {
  if (!XMemoCapBytes()) return nullptr;
  std::lock_guard<std::mutex> l(g_xmMu);
  auto it = g_xm.find(key);
  return it == g_xm.end() ? nullptr : it->second;
}
void XMemoPut(const std::string& key, const std::vector<uint8_t>& spv, const std::vector<uint8_t>& js) {
  const uint64_t cap = XMemoCapBytes();
  if (!cap) return;
  std::lock_guard<std::mutex> l(g_xmMu);
  if (g_xm.count(key) || g_xmBytes + spv.size() + js.size() > cap) return;
  auto e = std::make_shared<XMemoEntry>();
  e->spv = spv; e->js = js;
  g_xmBytes += spv.size() + js.size();
  g_xm.emplace(key, std::move(e));
}

// Reads every complete entry of the cache directory into the memo on a background thread, so the loader threads find them in memory (the first
// read of each of the 1,650 entries was most of what was left of the file time). An entry is memoized only after the checks tryLoad makes.
// SNAP_XCACHE_PREFETCH=0 turns it off.
void XMemoPrefetch(const std::string& dir) {
  const char* e = getenv("SNAP_XCACHE_PREFETCH");
  if (!XMemoCapBytes() || (e && *e == '0')) return;
  std::thread([dir] {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA((dir + "\\*.json").c_str(), &fd);
    if (fh == INVALID_HANDLE_VALUE) return;
    std::vector<std::string> keys;
    do {
      const std::string n = fd.cFileName;
      if (n.size() > 5 && n.find('.') == n.size() - 5) keys.push_back(n.substr(0, n.size() - 5));   // <key>.json (scratch files have more dots)
    } while (FindNextFileA(fh, &fd));
    FindClose(fh);
    size_t loaded = 0;
    for (const std::string& key : keys) {
      if (XMemoFind(key)) continue;
      std::vector<uint8_t> spv, js;
      if (!ReadFile(dir + "\\" + key + ".spv", &spv) || !ReadFile(dir + "\\" + key + ".json", &js)) continue;
      if (spv.size() < 20 || (spv.size() & 3) != 0) continue;
      J m;
      JP jp{reinterpret_cast<const char*>(js.data()), reinterpret_cast<const char*>(js.data()) + js.size()};
      if (!jp.parse(m)) continue;
      uint32_t co = 0;
      const bool haveCo = xcache::ReadCo(dir + "\\" + key + ".co", &co);
      XMemoPut(key, spv, js);
      if (haveCo) { std::lock_guard<std::mutex> l(g_xmMu); g_xmCo.emplace(key, co); }
      loaded++;
    }
    fprintf(stderr, "[nvn] shader cache prefetch: %zu entries in memory (%llu MB)\n", loaded, (unsigned long long)(g_xmBytes >> 20));
  }).detach();
}

// One stage of a ProgramSetShaders call, read out of guest memory at the call: the control section is only valid during the call, and a
// program built in the background (ProgramSetShadersImpl) works on these copies. The bytes are the ones the old inline path read.
constexpr size_t kCtlCopy = 0x758;   // BuildStage reads control words up to 0x750..0x753
struct StageIn {
  std::vector<uint8_t> ctl, data;
  uint32_t constOff = 0, dataSize = 0, codeOff = 0;
  int stage = 5;
  std::string key;
};

bool PrepStage(Host* h, const ShaderData& sd, StageIn* in) {
  static std::once_flag prefetchOnce;
  std::call_once(prefetchOnce, [&] { XMemoPrefetch(h->cacheDir); });
  PoolImpl* pool = nullptr; uint64_t off = 0;
  if (!h->ResolveAddress(sd.data, &pool, &off) || !pool->storage) {
    LogError("ProgramSetShaders: shader address 0x%llx is not in a pool", (unsigned long long)sd.data);
    return false;
  }
  if (!sd.control) { LogError("ProgramSetShaders: null control section"); return false; }
  const uint8_t* data = static_cast<const uint8_t*>(pool->storage) + off;
  const uint32_t progSize = Rd32(sd.control, 0x6F8), constOff = Rd32(sd.control, 0x700), dataSize = Rd32(sd.control, 0x704),
                 hdrSize = Rd32(sd.control, 0x708);
  if (dataSize == 0 || dataSize > (16u << 20) || constOff > dataSize || progSize > dataSize) {
    LogError("ProgramSetShaders: bad control section (size 0x%x const 0x%x)", dataSize, constOff);
    return false;
  }
  bool compute = Rd32(data, 0) != 0x12345678u;
  int stage = 5;
  uint32_t codeOff = 0;
  if (!compute) {
    codeOff = hdrSize;
    const uint32_t w0 = Rd32(data, codeOff);
    switch ((w0 >> 10) & 15) {   // SPH stage field [analysis/shaders/nushdb_layout.md]
      case 1: stage = 0; break;
      case 2: stage = 3; break;
      case 3: stage = 4; break;
      case 4: stage = 2; break;
      case 5: stage = 1; break;
      default: LogError("ProgramSetShaders: unknown SPH stage %u", (w0 >> 10) & 15); return false;
    }
  }
  // The key hashes the data section, stage and codeOff; constOff is recorded next to the entry and only changes the key on a mismatch (nvn_xcache.h).
  in->key = xcache::ResolveKeyWith([&](const std::string& extra) { return Sha256Hex(data, dataSize, extra); }, TranslatorSaltStr(), stage, codeOff, constOff,
                                   [&](const std::string& k, uint32_t* have) { return XMemoCo(h->cacheDir, k, have); });
  if (in->key.empty()) return false;
  in->ctl.assign(static_cast<const uint8_t*>(sd.control), static_cast<const uint8_t*>(sd.control) + kCtlCopy);
  in->data.assign(data, data + dataSize);
  in->constOff = constOff; in->dataSize = dataSize; in->codeOff = codeOff; in->stage = stage;
  return true;
}

// The stage's translation is already in the cache (in memory or on disk), so building it needs no translator.
bool StageCached(Host* h, const StageIn& in) {
  if (XMemoFind(in.key)) return true;
  return GetFileAttributesA((h->cacheDir + "\\" + in.key + ".json").c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool BuildStage(Host* h, const StageIn& in, StageProg* out) {
  const uint8_t* data = in.data.data();
  const void* ctl = in.ctl.data();
  const uint32_t constOff = in.constOff, dataSize = in.dataSize, codeOff = in.codeOff;
  const int stage = in.stage;
  const std::string& key = in.key;
  bool coKnown = false;
  if (XMemoCapBytes()) { std::lock_guard<std::mutex> l(g_xmMu); auto it = g_xmCo.find(key); coKnown = it != g_xmCo.end() && it->second == constOff; }
  const std::string base = h->cacheDir + "\\" + key;
  const std::string spvPath = base + ".spv", jsonPath = base + ".json";
  std::vector<uint8_t> spv, js;
  // cache I/O (2026-11-16): one writer per key and private scratch files (nvn_xcache.h); entries are published by rename (json last) and validated on load,
  // so a kill mid-write cannot leave a permanent bad entry, and two threads or processes on one cold cache cannot delete each other's files.
  J m;
  const bool loaded = xcache::EnsureEntry(
      h->cacheDir, key, constOff,
      [&]() {   // a complete, valid entry on disk
        if (auto e = XMemoFind(key)) {   // read and validated earlier in this run
          spv = e->spv; js = e->js;
          JP jp{reinterpret_cast<const char*>(js.data()), reinterpret_cast<const char*>(js.data()) + js.size()};
          m = J();
          if (jp.parse(m)) return true;
        }
        if (!ReadFile(spvPath, &spv) || !ReadFile(jsonPath, &js)) return false;
        if (spv.size() < 20 || (spv.size() & 3) != 0) { LogError("bad shader cache entry %s (spv %zu bytes); retranslating", base.c_str(), spv.size()); return false; }
        JP jp{reinterpret_cast<const char*>(js.data()), reinterpret_cast<const char*>(js.data()) + js.size()};
        m = J();
        if (jp.parse(m)) { XMemoPut(key, spv, js); return true; }
        LogError("bad shader manifest %s; retranslating", jsonPath.c_str());
        return false;
      },
      [&](const std::string& priv) {   // priv: this request's own path prefix
        const std::string binPath = priv + ".bin";
        WriteFile(binPath, data, dataSize);
        Log("translating shader %s (stage %d)", key.c_str(), stage);
        const uint64_t xt0 = snap_perf::now_ns();
        const bool ok = RunHarness(binPath, priv + ".spv", priv + ".json", stage, constOff, codeOff);
        snap_perf::event("xlate", (double)(snap_perf::now_ns() - xt0) / 1e6, key.c_str());   // SNAP_PERF events.csv: one row per cache miss, the time the translation blocked this thread
        if (ok) {   // first-launch progress line (a warm cache never gets here)
          static std::atomic<int> translated{0};
          int n = ++translated;
          if (n == 1 || n % 100 == 0) fprintf(stderr, "[nvn] first-run shader translation: %d shaders translated so far (cache: %s)\n", n, h->cacheDir.c_str());
        }
        return ok;
      }, nullptr, coKnown);
  if (!loaded) { LogError("shader cache entry %s unusable after retranslation", base.c_str()); return false; }
  XMemoSetCo(key, constOff);
  StageProg sp;
  sp.valid = true; sp.stage = stage; snprintf(sp.key, sizeof sp.key, "%.12s", key.c_str()); sp.fragOutputMap = m.i("fragmentOutputMap");
  snprintf(sp.fkey, sizeof sp.fkey, "%s", key.c_str());
  PipelineRegisterStageKey(sp.key, sp.fkey, stage == 1 && (Rd32(ctl, 0x718) & 1));
  if (auto* a = m.get("cbuffers")) for (auto& c : a->arr) sp.cb.push_back({c.i("slot"), c.i("set"), c.i("binding")});
  if (auto* a = m.get("sbuffers"))
    for (auto& c : a->arr) sp.sb.push_back({c.i("slot"), c.i("set"), c.i("binding"), c.i("sbCbSlot"), c.i("sbCbOffset"), c.i("write")});
  auto readTex = [](const J* a, std::vector<MTex>& v) {
    if (!a) return;
    for (auto& c : a->arr)
      v.push_back({c.i("set"), c.i("binding"), c.i("handleIndex"), c.i("cbufSlot", -1), c.i("type"), std::max(c.i("array", 1), 1), c.i("separate")});
  };
  readTex(m.get("textures"), sp.tex);
  readTex(m.get("images"), sp.img);
  std::vector<uint32_t> words(spv.size() / 4);
  memcpy(words.data(), spv.data(), words.size() * 4);
  if (stage == 5) {   // workgroup size: u32 x, y, z at control+0x748 (checked on the three resource.drp compute programs: 1x1x1, 16x16x1, 16x16x1)
    const uint32_t lx = Rd32(ctl, 0x748), ly = Rd32(ctl, 0x74C), lz = Rd32(ctl, 0x750);
    if (lx && ly && lz && lx <= 1024 && ly <= 1024 && lz <= 64 && lx * ly * lz <= 1024) { SpvSetLocalSize(words, lx, ly, lz); Log("compute shader %s: local size %ux%ux%u", key.c_str(), lx, ly, lz); }
    else LogError("compute shader %s: implausible local size %u %u %u in the control section", key.c_str(), lx, ly, lz);
  }
  if (stage == 1 && (Rd32(ctl, 0x718) & 1)) {   // layout(early_fragment_tests): see SpvAddEarlyFragmentTests
    if (SpvAddEarlyFragmentTests(words)) Log("fragment shader %s: early fragment tests", key.c_str());
    std::vector<uint8_t> mark;
    if (!ReadFile(base + ".efz", &mark)) WriteFile(base + ".efz", "1", 1);   // the cache key hashes only the data section, so the pipeline service needs this marker
  }
  SpvAddStorageImageCaps(words);   // the translator omits StorageImageRead/WriteWithoutFormat for storage images of unknown format
  SpvSaltForTest(words);
  if (stage == 1) GtaoPatchFragment(words, key.c_str());   // SNAP_SSAO_GTAO (nvn_gtao.cpp)
  if (stage == 1) AaPatchFragment(words, key.c_str());   // SNAP_AA=off/smaa: FXAA tonemap shaders lose their edge test (nvn_aa.cpp)
  if (stage == 0) snap_rt::PatchVertexXfb(words, key.c_str());   // [rt] SNAP_RT=1: gl_Position transform feedback decorations (no-op otherwise)
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = words.size() * 4; ci.pCode = words.data();
  if (vkCreateShaderModule(h->device, &ci, nullptr, &sp.mod) != VK_SUCCESS) { LogError("vkCreateShaderModule failed for %s", key.c_str()); return false; }
  NoteSubgroup(sp.mod, words);
  if (stage == 0) sp.inputMask = SpvVertexInputMask(words);
  if (stage == 1) { sp.spv = words; sp.outLocs = SpvFragmentOutputs(words); }
  if (stage == 0 && FoliageSwayKeepSpv()) sp.spv = words;   // vertex program kept for the wind sway wrapper (nvn_foliage.cpp)
  if (stage == 1 && getenv("SNAP_XLATE_AB")) LoadAbAlt(h, data, dataSize, stage, codeOff, constOff, (Rd32(ctl, 0x718) & 1) != 0, key, &sp);
  // c[1]: the constant-data blob that follows the code (data section tail)
  sp.constSize = dataSize - constOff;
  bool needC1 = false;
  for (auto& c : sp.cb) if (c.slot == 1) needC1 = true;
  if (needC1 && sp.constSize) {
    void* mp = nullptr;
    if (CreateHostBuffer(h, (sp.constSize + 15) & ~15ull, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &sp.constBuf, &sp.constMem, &mp))
      memcpy(mp, data + constOff, sp.constSize);
    else LogError("shader %s: its c[1] constant buffer could not be created; the stage has no c[1] data", key.c_str());
  }
  *out = std::move(sp);
  return true;
}

// Rebuilds a stage object (module, reflection, no constant buffer) from the translation cache by its full key. Used by the pipeline
// service to compile pipelines without the game having created the program. Compute stages are not supported.
bool LoadStageFromCacheImpl(Host* h, const std::string& key, StageProg* out, bool eft, bool* eftApplied) {
  std::vector<uint8_t> spv, js;
  const std::string base = h->cacheDir + "\\" + key;
  if (!ReadFile(base + ".spv", &spv) || !ReadFile(base + ".json", &js)) return false;
  J m;
  JP jp{reinterpret_cast<const char*>(js.data()), reinterpret_cast<const char*>(js.data()) + js.size()};
  if (!jp.parse(m)) return false;
  StageProg sp;
  sp.stage = m.i("stage", -1);
  if (sp.stage < 0 || sp.stage > 4) return false;
  sp.valid = true; snprintf(sp.key, sizeof sp.key, "%.12s", key.c_str()); snprintf(sp.fkey, sizeof sp.fkey, "%s", key.c_str());
  sp.fragOutputMap = m.i("fragmentOutputMap");
  if (auto* a = m.get("cbuffers")) for (auto& c : a->arr) sp.cb.push_back({c.i("slot"), c.i("set"), c.i("binding")});
  if (auto* a = m.get("sbuffers"))
    for (auto& c : a->arr) sp.sb.push_back({c.i("slot"), c.i("set"), c.i("binding"), c.i("sbCbSlot"), c.i("sbCbOffset"), c.i("write")});
  auto readTex = [](const J* a, std::vector<MTex>& v) {
    if (!a) return;
    for (auto& c : a->arr)
      v.push_back({c.i("set"), c.i("binding"), c.i("handleIndex"), c.i("cbufSlot", -1), c.i("type"), std::max(c.i("array", 1), 1), c.i("separate")});
  };
  readTex(m.get("textures"), sp.tex);
  readTex(m.get("images"), sp.img);
  std::vector<uint32_t> words(spv.size() / 4);
  memcpy(words.data(), spv.data(), words.size() * 4);
  { std::vector<uint8_t> mark; if (sp.stage == 1 && (eft || ReadFile(base + ".efz", &mark)) && SpvAddEarlyFragmentTests(words) && eftApplied) *eftApplied = true; }
  SpvAddStorageImageCaps(words);
  SpvSaltForTest(words);
  if (sp.stage == 1) GtaoPatchFragment(words, key.c_str());
  if (sp.stage == 1) AaPatchFragment(words, key.c_str());   // same patch for pipelines built by the pipeline service
  if (sp.stage == 0) snap_rt::PatchVertexXfb(words, key.c_str());   // [rt]
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = words.size() * 4; ci.pCode = words.data();
  if (vkCreateShaderModule(h->device, &ci, nullptr, &sp.mod) != VK_SUCCESS) return false;
  NoteSubgroup(sp.mod, words);
  if (sp.stage == 0) sp.inputMask = SpvVertexInputMask(words);
  if (sp.stage == 1) { sp.spv = words; sp.outLocs = SpvFragmentOutputs(words); }
  if (sp.stage == 0 && FoliageSwayKeepSpv()) sp.spv = words;
  *out = std::move(sp);
  return true;
}

}  // namespace

bool LoadStageFromCache(Host* h, const std::string& fullKey, StageProg* out, bool eft, bool* eftApplied) { return LoadStageFromCacheImpl(h, fullKey, out, eft, eftApplied); }
const char* TranslatorVersion() { return TranslatorSaltStr().c_str(); }
bool ModuleUsesSubgroup(VkShaderModule m) { std::lock_guard<std::mutex> l(g_sgMu); return g_sgMods.count((uint64_t)(uintptr_t)m) != 0; }

// A host-visible mapped buffer (the steps and their failure handling live in nvn_hostbuf.h, which tests/nvn/hostbuf_test.cpp drives with a
// failing fake). True only when created, bound and mapped; a failed step is logged to stderr (so the problem report has it).
bool CreateHostBuffer(Host* h, uint64_t size, VkBufferUsageFlags usage, VkBuffer* buf, VkDeviceMemory* mem, void** mapped) {
  struct RealOps {
    Host* h; uint64_t size;
    VkResult CreateBuffer(const VkBufferCreateInfo* i, VkBuffer* b) { return vkCreateBuffer(h->device, i, nullptr, b); }
    void MemReqs(VkBuffer b, VkMemoryRequirements* r) { vkGetBufferMemoryRequirements(h->device, b, r); }
    uint32_t MemType(uint32_t bits) { return h->FindMemoryType(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); }
    VkResult Allocate(const VkMemoryAllocateInfo* i, VkDeviceMemory* m) { return vkAllocateMemory(h->device, i, nullptr, m); }
    VkResult Bind(VkBuffer b, VkDeviceMemory m) { return vkBindBufferMemory(h->device, b, m, 0); }
    VkResult Map(VkDeviceMemory m, void** p) { return vkMapMemory(h->device, m, 0, VK_WHOLE_SIZE, 0, p); }
    void DestroyBuffer(VkBuffer b) { vkDestroyBuffer(h->device, b, nullptr); }
    void Free(VkDeviceMemory m) { vkFreeMemory(h->device, m, nullptr); }
    void Error(const char* step, VkResult r) { LogError("host buffer of %llu bytes: %s failed: %d", (unsigned long long)size, step, (int)r); }
  } ops{h, size};
  return snapnvn::CreateHostBufferWith(ops, size, usage, buf, mem, mapped);
}

// ---- Programs built in the background (2026-11-23, worker fastshaders; docs/shader-prebuild.md "First launch") ----------------------------------
// The game creates every program at boot (about 6,200 from about 1,650 distinct shaders). With an empty translation cache each one used to be
// translated inline, one after another, on the loader thread: 358 s to the title on the first launch against 6.4 s with a warm cache. Now a
// program whose shader is not in the cache is queued and ProgramSetShaders returns at once; XlateWorkers() threads, each with its own translator
// process, build the queue in order. Nothing is ever drawn without its shader: every use of a program goes through ProgramResolve (draw replay,
// the persistent draw state) or ProgramDestroy, and both wait for the program first. A program that is still queued when it is needed is built
// on the waiting thread at once instead of waiting its turn. The translation and the files it writes are the same as on the inline path.
struct ProgramJob {
  ProgramImpl* p = nullptr;
  std::vector<StageIn> in;
  std::atomic<int> state{0};   // 0 queued, 1 building, 2 done
  std::atomic<bool> hinted{false};   // moved to the front of the queue once (ProgramHint)
  std::mutex mu;
  std::condition_variable cv;
};
namespace {
std::mutex& g_jobMu = *new std::mutex;   // ProgramImpl::job
std::atomic<int> g_jobsOpen{0};           // queued or building; 0 = nothing to wait for
std::mutex& g_qMu = *new std::mutex;
std::condition_variable& g_qCv = *new std::condition_variable;
auto& g_q = *new std::deque<std::shared_ptr<ProgramJob>>;
int g_workers = 0;
struct JobStats { uint64_t firstNs = 0; int programs = 0, stolen = 0; };
JobStats g_jobStats;   // under g_qMu
std::atomic<bool> g_jobStop{false};   // device shutdown: no new builds (ProgramJobsShutdown)
std::atomic<int> g_jobBuilding{0};

// Moves j from queued to building for this thread. False when another thread has it or the device is shutting down.
bool TakeJob(const std::shared_ptr<ProgramJob>& j) {
  g_jobBuilding.fetch_add(1);
  int z = 0;
  if (!g_jobStop.load() && j->state.compare_exchange_strong(z, 1)) return true;
  g_jobBuilding.fetch_sub(1);
  return false;
}

void ProgramDestroyNow(ProgramImpl* p);
bool BuildProgram(ProgramImpl* p, const std::vector<StageIn>& in) {
  Host* h = p->host;
  for (const StageIn& si : in) {
    StageProg sp;
    if (!BuildStage(h, si, &sp)) { ProgramDestroyNow(p); return false; }
    if (p->st[sp.stage].valid) {
      LogError("ProgramSetShaders: two shaders for stage %d", sp.stage);
      FreeStage(h, sp); ProgramDestroyNow(p);
      return false;
    }
    p->st[sp.stage] = std::move(sp);
  }
  return ProgramBuildLayout(p);
}

void RunJob(const std::shared_ptr<ProgramJob>& j) {   // after TakeJob
  if (!BuildProgram(j->p, j->in)) LogError("a program built in the background failed: it draws nothing (the inline path would have returned false to the game)");
  j->in.clear(); j->in.shrink_to_fit();
  { std::lock_guard<std::mutex> lk(j->mu); j->state.store(2, std::memory_order_release); }
  j->cv.notify_all();
  g_jobBuilding.fetch_sub(1);
  if (g_jobsOpen.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    std::lock_guard<std::mutex> lk(g_qMu);   // the backlog is done (a later miss starts a new one)
    fprintf(stderr, "[nvn] background shader build: done, %d programs built by %d workers in %.1f s (%d of them built early for a draw)\n", g_jobStats.programs,
            g_workers, (double)(snap_perf::now_ns() - g_jobStats.firstNs) / 1e9, g_jobStats.stolen);
  }
}

void JobWorker() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  for (;;) {
    std::shared_ptr<ProgramJob> j;
    {
      std::unique_lock<std::mutex> lk(g_qMu);
      g_qCv.wait(lk, [] { return !g_q.empty(); });
      j = std::move(g_q.front());
      g_q.pop_front();
    }
    if (TakeJob(j)) RunJob(j);   // else a waiting thread took it (or the device is going away)
  }
}

void Enqueue(ProgramImpl* p, std::vector<StageIn>&& in) {
  auto j = std::make_shared<ProgramJob>();
  j->p = p; j->in = std::move(in);
  const bool first = g_jobsOpen.fetch_add(1, std::memory_order_acq_rel) == 0;
  { std::lock_guard<std::mutex> lk(g_jobMu); p->job = j; }
  {
    std::lock_guard<std::mutex> lk(g_qMu);
    if (first) g_jobStats = JobStats{snap_perf::now_ns(), 0, 0};
    g_jobStats.programs++;
    while (g_workers < XlateWorkers()) { std::thread(JobWorker).detach(); g_workers++; }
    g_q.push_back(std::move(j));
  }
  g_qCv.notify_one();
}

}  // namespace

// The game recorded a bind of p (CommandBufferBindProgram): if p is still queued, move it to the front so the workers build it before the
// replay needs it. A frame binds many programs; without this the replay thread built them one after another itself.
void ProgramHint(ProgramImpl* p) {
  if (!p || g_jobsOpen.load(std::memory_order_acquire) == 0) return;
  std::shared_ptr<ProgramJob> j;
  { std::lock_guard<std::mutex> lk(g_jobMu); j = p->job; }
  if (!j || j->state.load(std::memory_order_acquire) != 0 || j->hinted.exchange(true)) return;
  { std::lock_guard<std::mutex> lk(g_qMu); g_q.push_front(std::move(j)); }   // the copy further back is skipped when a worker reaches it
  g_qCv.notify_one();
}

namespace {
// Returns when p is fully built (or its build failed). Cheap when nothing is queued.
void ProgramWait(ProgramImpl* p) {
  if (!p || g_jobsOpen.load(std::memory_order_acquire) == 0) return;
  std::shared_ptr<ProgramJob> j;
  { std::lock_guard<std::mutex> lk(g_jobMu); j = p->job; }
  if (!j) return;
  if (g_jobStop.load()) return;   // shutting down: the program stays empty
  if (TakeJob(j)) {   // still queued: build it here, now
    { std::lock_guard<std::mutex> lk(g_qMu); g_jobStats.stolen++; }
    RunJob(j);
  } else {
    std::unique_lock<std::mutex> lk(j->mu);   // another thread builds it (or, while shutting down, nobody will: stop waiting then)
    while (!j->cv.wait_for(lk, std::chrono::milliseconds(20), [&] {
      const int st = j->state.load(std::memory_order_acquire);
      return st == 2 || (st == 0 && g_jobStop.load());
    })) {}
  }
  std::lock_guard<std::mutex> lk(g_jobMu);
  if (p->job == j) p->job.reset();
}

}  // namespace

// Host::Shutdown, before the device goes: queued programs are not built any more and the builds under way finish.
void ProgramJobsShutdown() {
  g_jobStop.store(true);
  for (int i = 0; i < 20000 && g_jobBuilding.load() > 0; i++) Sleep(1);
}

void ProgramDestroy(ProgramImpl* p) {
  ProgramWait(p);
  ProgramDestroyNow(p);
}

namespace {
void ProgramDestroyNow(ProgramImpl* p) {
  if (!p) return;
  Host* h = p->host;
  if (h && h->device) {
    // pipelines built from this program stay in the host map (keyed by program id, ids are never reused) and are
    // destroyed at shutdown
    if (!p->composite && p->id) {   // composites built from this program hold copies of its stage objects
      std::lock_guard<std::mutex> lk(h->compMutex);
      for (auto it = h->composites.begin(); it != h->composites.end();) {
        bool uses = false;
        for (uint64_t k : it->first) uses |= k == p->id;
        if (uses) { ProgramDestroy(it->second); delete it->second; it = h->composites.erase(it); } else ++it;
      }
    }
    if (!p->composite) for (auto& s : p->st) FreeStage(h, s);
    for (auto l : p->setLayout) if (l) vkDestroyDescriptorSetLayout(h->device, l, nullptr);
    if (p->layout) vkDestroyPipelineLayout(h->device, p->layout, nullptr);
  }
  p->layout = VK_NULL_HANDLE;
  for (auto& l : p->setLayout) l = VK_NULL_HANDLE;
  for (auto& u : p->setUsed) u = false;
}
}  // namespace

// Binding model (see the harness): set 0 = support buffer (0) + c[s] of stage at 1 + stage*16 + s; set 1 = storage
// buffers stage*16 + n; set 2 = combined image samplers stage*32 + n; set 3 = storage images stage*8 + n.
bool ProgramSetShadersImpl(ProgramImpl* p, int n, const ShaderData* sd) {
  Host* h = p->host;
  snap_perf::Scope perfShader(snap_perf::C_SHADER); snap_perf::count(snap_perf::V_SHADERS);
  ProgramDestroy(p);   // waits for an earlier background build of this program
  if (n < 1 || n > 6 || !sd) return false;
  std::vector<StageIn> in(n);
  bool miss = false;
  for (int i = 0; i < n; i++) {
    if (!PrepStage(h, sd[i], &in[i])) return false;
    if (!miss && !StageCached(h, in[i])) miss = true;
  }
  if (miss && XlateWorkers() > 1) { Enqueue(p, std::move(in)); return true; }   // see ProgramJob
  return BuildProgram(p, in);
}

bool ProgramBuildLayout(ProgramImpl* p) {
  Host* h = p->host;
  struct B { VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; uint32_t count = 0; VkShaderStageFlags stages = 0; };
  std::map<std::pair<int, int>, B> binds;
  VkShaderStageFlags all = 0;
  for (auto& s : p->st) if (s.valid) all |= StageBit(s.stage);
  binds[{0, 0}] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, all};
  auto add = [&](int set, int binding, VkDescriptorType t, uint32_t cnt, VkShaderStageFlags st) {
    auto& b = binds[{set, binding}];
    b.type = t; b.count = std::max(b.count, cnt); b.stages |= st;
  };
  for (auto& s : p->st) {
    if (!s.valid) continue;
    VkShaderStageFlags sb = StageBit(s.stage);
    for (auto& c : s.cb) add(c.set, c.binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, sb);
    for (auto& c : s.sb) add(c.set, c.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sb);
    for (auto& t : s.tex) {
      if (t.separate) { LogError("separate texture/sampler bindings are not implemented yet"); continue; }
      add(t.set, t.binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)t.array, sb);
    }
    for (auto& t : s.img) add(t.set, t.binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (uint32_t)t.array, sb);
  }
  for (int set = 0; set < 4; set++) {
    std::vector<VkDescriptorSetLayoutBinding> v;
    for (auto& kv : binds)
      if (kv.first.first == set) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = (uint32_t)kv.first.second; b.descriptorType = kv.second.type; b.descriptorCount = kv.second.count;
        b.stageFlags = kv.second.stages;
        v.push_back(b);
      }
    p->setUsed[set] = !v.empty();
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = (uint32_t)v.size(); lci.pBindings = v.data();
    if (vkCreateDescriptorSetLayout(h->device, &lci, nullptr, &p->setLayout[set]) != VK_SUCCESS) { ProgramDestroy(p); return false; }
  }
  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount = 4; pl.pSetLayouts = p->setLayout;
  const uint32_t pomRange = PomPushRangeBytes(h->props.limits.maxPushConstantsSize);   // parallax (nvn_pom.cpp) uses bytes 128..255 of the same fragment range
  const VkPushConstantRange rtRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, pomRange ? pomRange : (uint32_t)sizeof(snap_rt::ShadowPush)};   // [rt] M3: the RT shadow receivers' ray-query constants (nvn_shadowrt.cpp); every layout carries it so push constants survive pipeline changes
  if (snap_rt::Capture() || pomRange) { pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &rtRange; }
  if (vkCreatePipelineLayout(h->device, &pl, nullptr, &p->layout) != VK_SUCCESS) { ProgramDestroy(p); return false; }
  {
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    p->id = h->nextProgramId++;
  }
  return true;
}

ProgramImpl* ProgramResolve(Host* h, ProgramImpl* const src[6]) {
  for (int s = 0; s < 6; s++) ProgramWait(src[s]);   // a program built in the background is complete before anything reads it
  ProgramImpl* first = nullptr;
  int bound = 0;
  std::array<uint64_t, 6> key{};
  for (int s = 0; s < 6; s++) {
    if (!src[s]) continue;
    if (!first) first = src[s];
    if (!src[s]->st[s].valid) continue;
    bound |= 1 << s;
    key[s] = src[s]->id;
  }
  if (!bound) return first;   // nothing usable: hand back any bound program so the draw reports it
  ProgramImpl* one = nullptr;
  bool single = true;
  for (int s = 0; s < 6; s++) if (bound & (1 << s)) { if (!one) one = src[s]; else if (one != src[s]) single = false; }
  if (single) for (int s = 0; s < 6; s++) if (one->st[s].valid && !(bound & (1 << s))) single = false;
  if (single) return one;
  std::lock_guard<std::mutex> lk(h->compMutex);
  auto it = h->composites.find(key);
  if (it != h->composites.end()) return it->second;
  auto* c = new ProgramImpl;
  c->host = h; c->composite = true;
  for (int s = 0; s < 6; s++) if (bound & (1 << s)) c->st[s] = src[s]->st[s];
  if (!ProgramBuildLayout(c)) { ProgramDestroy(c); delete c; return one; }
  h->composites[key] = c;
  return c;
}

}  // namespace nvn
