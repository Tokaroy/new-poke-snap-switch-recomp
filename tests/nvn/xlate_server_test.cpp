// Persistent translator client (runtime/nvn/nvn_xlate_server.h). The test binary is its own fake translator: run with `--fake <mode>` it answers the
// serve protocol (one request line in, one reply line out). No GPU, game data or .NET needed.
#define NOMINMAX
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "nvn_xlate_server.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

static int Fake(const std::string& mode) {
  std::string line;
  int n = 0;
  if (mode == "exit") return 3;                 // an older harness: no serve mode, ends at once
  while (std::getline(std::cin, line)) {
    n++;
    if (mode == "die_second" && n == 2) return 9;   // dies while a request is open
    if (mode == "hang") { Sleep(60000); return 0; }
    if (mode == "slow") Sleep(30);
    if (line.find("reject") != std::string::npos) { printf("err\tbad shader: x\n"); fflush(stdout); continue; }
    // field 2 is the spv path: leave a file there like the real translator
    size_t a = line.find('\t'), b = line.find('\t', a + 1);
    std::string spv = line.substr(a + 1, b - a - 1);
    FILE* f = fopen(spv.c_str(), "wb"); if (f) { fputs("ok", f); fclose(f); }
    printf("ok\n"); fflush(stdout);
  }
  return 0;
}

static std::string Self() { char p[MAX_PATH]; GetModuleFileNameA(nullptr, p, MAX_PATH); return p; }
static std::string Cmd(const char* mode) { return "\"" + Self() + "\" --fake " + mode; }

int main(int argc, char** argv) {
  if (argc == 3 && !strcmp(argv[1], "--fake")) return Fake(argv[2]);
  using nvn::xlate::Server;
  char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
  const std::string dir = std::string(tmp) + "xlate_server_test_" + std::to_string(GetCurrentProcessId());
  CreateDirectoryA(dir.c_str(), nullptr);
  auto P = [&](const char* n) { return dir + "\\" + n; };
  std::string msg;

  {  // ok, reject, ok again: the reply is matched to the request, the process stays up
    Server s(Cmd("echo"));
    CHECK(s.Translate("a.bin", P("a.spv"), P("a.json"), 1, 0, 48, &msg) == Server::Result::Ok);
    CHECK(GetFileAttributesA(P("a.spv").c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(s.Translate("reject.bin", P("r.spv"), P("r.json"), 1, 0, 48, &msg) == Server::Result::Failed);
    CHECK(msg.find("bad shader") != std::string::npos);
    CHECK(s.Translate("b.bin", P("b.spv"), P("b.json"), 0, 4864, 0) == Server::Result::Ok);
    CHECK(s.starts() == 1 && s.served() == 3 && s.running());
  }
  {  // an executable that is no server (exits at once): Unavailable, and not tried again
    Server s(Cmd("exit"));
    CHECK(s.Translate("a.bin", P("c.spv"), P("c.json"), 1, 0, 0) == Server::Result::Unavailable);
    CHECK(s.disabled());
    CHECK(s.Translate("a.bin", P("c.spv"), P("c.json"), 1, 0, 0) == Server::Result::Unavailable);
    CHECK(s.starts() == 1);
  }
  {  // a command that cannot start
    Server s("\"C:\\no\\such\\harness.exe\" --nvn-serve");
    CHECK(s.Translate("a.bin", P("d.spv"), P("d.json"), 1, 0, 0) == Server::Result::Unavailable);
    CHECK(s.disabled());
  }
  {  // dies mid-request after one good answer: that request falls back, the next one restarts the process
    Server s(Cmd("die_second"));
    CHECK(s.Translate("a.bin", P("e1.spv"), P("e1.json"), 1, 0, 0) == Server::Result::Ok);
    CHECK(s.Translate("a.bin", P("e2.spv"), P("e2.json"), 1, 0, 0) == Server::Result::Unavailable);
    CHECK(!s.disabled());
    CHECK(s.Translate("a.bin", P("e3.spv"), P("e3.json"), 1, 0, 0) == Server::Result::Ok);
    CHECK(s.starts() == 2);
  }
  {  // a hung process: the request time limit ends it
    Server s(Cmd("hang"), 400);
    const DWORD t0 = GetTickCount();
    CHECK(s.Translate("a.bin", P("f.spv"), P("f.json"), 1, 0, 0) == Server::Result::Unavailable);
    CHECK(GetTickCount() - t0 < 10000);
    CHECK(!s.running());
  }
  {  // idle: the process is closed after the idle time and a later request starts a new one
    Server s(Cmd("echo"), 10000, 300);
    CHECK(s.Translate("a.bin", P("g.spv"), P("g.json"), 1, 0, 0) == Server::Result::Ok);
    Sleep(1200);
    CHECK(!s.running());
    CHECK(s.Translate("a.bin", P("g2.spv"), P("g2.json"), 1, 0, 0) == Server::Result::Ok);
    CHECK(s.starts() == 2);
  }
  {  // many threads: one process, every request answered, none mixed up
    Server s(Cmd("slow"));
    std::vector<std::thread> th; std::atomic<int> ok{0};
    for (int i = 0; i < 8; i++) th.emplace_back([&, i] {
      for (int j = 0; j < 5; j++) {
        std::string n = "t" + std::to_string(i) + "_" + std::to_string(j) + ".spv";
        if (s.Translate("a.bin", P(n.c_str()), P("x.json"), 1, 0, 0) == Server::Result::Ok &&
            GetFileAttributesA(P(n.c_str()).c_str()) != INVALID_FILE_ATTRIBUTES) ok++;
      }
    });
    for (auto& t : th) t.join();
    CHECK(ok == 40);
    CHECK(s.starts() == 1);
  }
  {  // the child ends with the client (job object): destroy while running, nothing left behind to hold the exe
    { Server s(Cmd("echo")); CHECK(s.Translate("a.bin", P("h.spv"), P("h.json"), 1, 0, 0) == Server::Result::Ok); }
  }

  // real harness, when one is built: a translator reply for a missing input file is an error reply, not a hang
  if (const char* h = getenv("NVN_SHADER_HARNESS")) {
    Server s(std::string("\"") + h + "\" --nvn-serve");
    CHECK(s.Translate(P("nope.bin"), P("z.spv"), P("z.json"), 1, 0, 0, &msg) == Server::Result::Failed);
    printf("real harness reply: %s\n", msg.c_str());
  }

  system(("rmdir /s /q \"" + dir + "\"").c_str());   // our own scratch folder, flat files only
  printf(g_fail ? "xlate_server_test: %d FAILED\n" : "xlate_server_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
