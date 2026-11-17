// Translation-cache entry protocol (runtime/nvn/nvn_xcache.h): private scratch files, one writer per key across threads and processes,
// recovery from a writer that died, and the constOff sidecar. No GPU, no game data. Build and run: tests/nvn/xcache_test.sh
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "nvn_xcache.h"

using namespace nvn::xcache;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static std::string Hex(const std::string& s) { char b[20]; snprintf(b, sizeof b, "%016llx", (unsigned long long)std::hash<std::string>{}(s)); return std::string(b) + std::string(b) + std::string(b) + std::string(b); }
static std::string ReadAll(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static int CountMatching(const std::string& dir, const std::string& pat) {
  int n = 0; WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((dir + "\\" + pat).c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do n++; while (FindNextFileA(h, &fd));
  FindClose(h); return n;
}
static std::string MakeDir(const char* name) {
  char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
  std::string d = std::string(tmp) + "snap_xcache_test_" + name + "_" + std::to_string(GetCurrentProcessId());
  DeleteMatching("*", d); RemoveDirectoryA(d.c_str()); CreateDirectoryA(d.c_str(), nullptr);
  return d;
}
static void Cleanup(const std::string& d) { DeleteMatching("*", d); RemoveDirectoryA(d.c_str()); }

// a valid entry: >= 20 bytes, multiple of 4, and a non-empty manifest
static auto Loader(const std::string& dir, const std::string& key) {
  return [dir, key]() { const std::string s = ReadAll(dir + "\\" + key + ".spv"), j = ReadAll(dir + "\\" + key + ".json"); return s.size() >= 20 && (s.size() & 3) == 0 && !j.empty(); };
}
static std::atomic<int> g_calls{0}, g_live{0}, g_maxLive{0};
static auto Translator(int sleepMs, const std::string& tag, bool ok = true) {
  return [=](const std::string& priv) {
    g_calls++; const int now = ++g_live; int m = g_maxLive.load(); while (now > m && !g_maxLive.compare_exchange_weak(m, now)) {}
    { std::ofstream f(priv + ".bin", std::ios::binary); f << "input"; }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    bool intact = FileExists(priv + ".bin");   // nobody removed this request's input while it ran
    if (ok) {
      std::ofstream s(priv + ".spv", std::ios::binary); s << std::string(24, 'S');
      std::ofstream j(priv + ".json", std::ios::binary); j << "{\"tag\":\"" << tag << "\"}";
    }
    --g_live;
    return ok && intact;
  };
}

static void TestSuffixAndCo() {
  std::set<std::string> seen;
  for (int i = 0; i < 1000; i++) seen.insert(UniqueSuffix());
  CHECK(seen.size() == 1000);
  CHECK(seen.begin()->find(".x" + std::to_string(GetCurrentProcessId()) + "_") == 0);

  const std::string d = MakeDir("co");
  auto hash = [](const std::string& extra) { return Hex("DATA|" + extra); };
  const std::string base = ResolveKey(d, hash, "salt", 1, 0x30, 0x100);
  CHECK(base == hash("salt|1|48"));                                   // no sidecar: the old key, so every existing entry keeps its name
  WriteCo(d + "\\" + base + ".co", 0x100);
  CHECK(ResolveKey(d, hash, "salt", 1, 0x30, 0x100) == base);        // sidecar matches: same key
  const std::string alt = ResolveKey(d, hash, "salt", 1, 0x30, 0x80);
  CHECK(alt != base && alt == hash("salt|1|48|co128"));              // same data, stage and codeOff, other constOff: another identity
  CHECK(ResolveKey(d, hash, "salt", 1, 0x30, 0x80) == alt);          // stable
  CHECK(ResolveKey(d, hash, "salt", 1, 0x30, 0x90) != alt);          // each constOff its own key
  CHECK(ResolveKey(d, hash, "salt", 2, 0x30, 0x80) == hash("salt|2|48"));   // stage still part of the identity
  { std::ofstream f(d + "\\" + base + ".co"); f << "garbage"; }
  CHECK(ResolveKey(d, hash, "salt", 1, 0x30, 0x80) == base);         // an unreadable sidecar counts as absent
  Cleanup(d);
}

static void TestOneWriterManyThreads() {
  const std::string d = MakeDir("threads"), key = Hex("k1");
  g_calls = 0; g_live = 0; g_maxLive = 0;
  std::vector<std::thread> th; std::atomic<int> loaded{0};
  for (int i = 0; i < 8; i++) th.emplace_back([&] { if (EnsureEntry(d, key, 0x100, Loader(d, key), Translator(120, "A"))) loaded++; });
  for (auto& t : th) t.join();
  CHECK(g_calls == 1);                       // one translation for eight requests of one cold key
  CHECK(g_maxLive == 1);
  CHECK(loaded == 8);
  CHECK(ReadAll(d + "\\" + key + ".json") == "{\"tag\":\"A\"}");
  CHECK(ReadAll(d + "\\" + key + ".co") == "256");
  CHECK(CountMatching(d, key + ".x*") == 0); // no scratch left behind
  Cleanup(d);
}

static void TestDifferentKeysRunTogether() {
  const std::string d = MakeDir("keys"), k1 = Hex("a"), k2 = Hex("b");
  g_calls = 0; g_live = 0; g_maxLive = 0;
  std::thread a([&] { EnsureEntry(d, k1, 1, Loader(d, k1), Translator(300, "1")); });
  std::thread b([&] { EnsureEntry(d, k2, 1, Loader(d, k2), Translator(300, "2")); });
  a.join(); b.join();
  CHECK(g_calls == 2 && g_maxLive == 2);     // the claim is per key: two keys translate side by side
  CHECK(Loader(d, k1)() && Loader(d, k2)());
  Cleanup(d);
}

static void TestWriterDoesNotLoseItsInput() {
  // writer A is inside its translation when writer B asks for the same key and finds a corrupt entry: B must wait, not delete A's files
  const std::string d = MakeDir("cross"), key = Hex("k2");
  g_calls = 0; g_live = 0; g_maxLive = 0;
  { std::ofstream f(d + "\\" + key + ".spv", std::ios::binary); f << "xx"; }   // corrupt final
  std::atomic<bool> aIn{false}, aOk{false}, bOk{false};
  std::thread a([&] {
    aOk = EnsureEntry(d, key, 7, Loader(d, key), [&](const std::string& priv) {
      aIn = true; g_calls++;
      { std::ofstream f(priv + ".bin", std::ios::binary); f << "in"; }
      std::this_thread::sleep_for(std::chrono::milliseconds(400));
      const bool intact = FileExists(priv + ".bin");
      { std::ofstream s(priv + ".spv", std::ios::binary); s << std::string(24, 'S'); std::ofstream j(priv + ".json", std::ios::binary); j << "{}"; }
      return intact;
    });
  });
  while (!aIn) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::thread b([&] { bOk = EnsureEntry(d, key, 7, Loader(d, key), Translator(10, "B")); });
  a.join(); b.join();
  CHECK(aOk && bOk);
  CHECK(g_calls == 1);                        // B found A's published entry after the claim and did not translate again
  CHECK(CountMatching(d, key + ".x*") == 0);
  Cleanup(d);
}

static void TestDeadWriter() {
  const std::string d = MakeDir("dead"), key = Hex("k3");
  // a writer that ends holding the claim (the mutex is abandoned) and leaves its scratch files
  std::thread t([&] {
    KeyClaim c(key); CHECK(c.held()); c.leak();
    { std::ofstream f(d + "\\" + key + ".x999_1.bin"); f << "x"; }
    { std::ofstream f(d + "\\" + key + ".x999_1.spv.tmp"); f << "x"; }
  });
  t.join();
  { KeyClaim c(key, 2000); CHECK(c.held() && c.abandoned()); }   // the next claimant gets it, and knows
  { std::ofstream f(d + "\\" + key + ".json"); f << "{}"; }      // half entry: json without spv
  g_calls = 0;
  const bool ok = EnsureEntry(d, key, 5, Loader(d, key), Translator(5, "R"));
  CHECK(ok && g_calls == 1);
  CHECK(ReadAll(d + "\\" + key + ".json") == "{\"tag\":\"R\"}");
  CHECK(CountMatching(d, key + ".x*") == 0);  // the dead writer's leftovers were swept
  Cleanup(d);
}

static void TestFailureAndLegacy() {
  const std::string d = MakeDir("fail"), key = Hex("k4");
  g_calls = 0;
  CHECK(!EnsureEntry(d, key, 1, Loader(d, key), Translator(5, "F", false)));
  CHECK(!FileExists(d + "\\" + key + ".spv") && !FileExists(d + "\\" + key + ".json") && !FileExists(d + "\\" + key + ".co"));
  CHECK(CountMatching(d, key + ".x*") == 0);
  CHECK(EnsureEntry(d, key, 1, Loader(d, key), Translator(5, "G")));   // a later request recovers
  // an entry from before the sidecar existed: loaded as it is, and the sidecar is added
  const std::string k5 = Hex("k5");
  { std::ofstream s(d + "\\" + k5 + ".spv", std::ios::binary); s << std::string(24, 'L'); std::ofstream j(d + "\\" + k5 + ".json"); j << "{}"; }
  g_calls = 0;
  CHECK(EnsureEntry(d, k5, 9, Loader(d, k5), Translator(5, "N")) && g_calls == 0);
  CHECK(ReadAll(d + "\\" + k5 + ".co") == "9");
  CHECK(ReadAll(d + "\\" + k5 + ".spv") == std::string(24, 'L'));
  Cleanup(d);
}

static int Child(const std::string& d, const std::string& key) {   // another process on the same cache folder
  const int rc = EnsureEntry(d, key, 3, Loader(d, key), [&](const std::string& priv) {
    { std::ofstream f(d + "\\translate.log", std::ios::app); f << GetCurrentProcessId() << "\n"; }
    { std::ofstream f(priv + ".bin", std::ios::binary); f << "in"; }
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    if (!FileExists(priv + ".bin")) return false;
    { std::ofstream s(priv + ".spv", std::ios::binary); s << std::string(24, 'P'); std::ofstream j(priv + ".json", std::ios::binary); j << "{}"; }
    return true;
  }) ? 0 : 2;
  return rc;
}

static void TestTwoProcesses(const char* self) {
  const std::string d = MakeDir("procs"), key = Hex("k6");
  HANDLE h[3] = {};
  for (int i = 0; i < 3; i++) {
    std::string cmd = std::string("\"") + self + "\" --child \"" + d + "\" " + key;
    STARTUPINFOA si{sizeof si}; PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { h[i] = pi.hProcess; CloseHandle(pi.hThread); }
  }
  int ok = 0;
  for (int i = 0; i < 3; i++) if (h[i]) { WaitForSingleObject(h[i], 30000); DWORD c = 99; GetExitCodeProcess(h[i], &c); if (c == 0) ok++; CloseHandle(h[i]); }
  CHECK(ok == 3);
  const std::string log = ReadAll(d + "\\translate.log");
  int lines = 0; for (char c : log) if (c == '\n') lines++;
  CHECK(lines == 1);                          // three processes, one translation
  CHECK(Loader(d, key)() && ReadAll(d + "\\" + key + ".co") == "3");
  CHECK(CountMatching(d, key + ".x*") == 0);
  Cleanup(d);
}

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "--child") return Child(argv[2], argv[3]);
  TestSuffixAndCo();
  TestOneWriterManyThreads();
  TestDifferentKeysRunTogether();
  TestWriterDoesNotLoseItsInput();
  TestDeadWriter();
  TestFailureAndLegacy();
  TestTwoProcesses(argv[0]);
  printf(g_fail ? "xcache_test: %d FAILED\n" : "xcache_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
