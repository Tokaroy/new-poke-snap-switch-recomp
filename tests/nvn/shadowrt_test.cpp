// SPDX-License-Identifier: MIT
// Offline check of PatchShadowRt (runtime/nvn/nvn_shadowrt.cpp): patches every translated fragment program that has a dynamic-shadow-map tap and writes the
// patched modules so spirv-val can check them. usage: shadowrt_test <cache dir with *.spv> <out dir> <rays>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include "nvn_internal.h"
namespace nvn { int g_presentCount = 0; }
namespace fs = std::filesystem;
int main(int argc, char** argv) {
  if (argc < 4) { fprintf(stderr, "usage: shadowrt_test <cache dir> <out dir> <rays>\n"); return 2; }
  const int rays = atoi(argv[3]);
  fs::create_directories(argv[2]);
  int seen = 0, patched = 0, kept = 0;
  for (auto& e : fs::directory_iterator(argv[1])) {
    if (e.path().extension() != ".spv") continue;
    std::ifstream f(e.path(), std::ios::binary); std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
    if (b.size() < 24 || b.size() % 4) continue;
    std::vector<uint32_t> w(b.size() / 4); memcpy(w.data(), b.data(), b.size());
    bool has = false;
    for (size_t i = 5; i < w.size();) { const uint32_t wc = w[i] >> 16; if (!wc) break; if ((w[i] & 0xFFFF) == 90) { has = true; break; } i += wc; }
    if (!has) continue;
    seen++;
    std::vector<uint32_t> out;
    if (nvn::PatchShadowRt(w, rays, out)) {
      patched++;
      std::ofstream o(fs::path(argv[2]) / e.path().filename(), std::ios::binary); o.write((const char*)out.data(), out.size() * 4);
    } else kept++;
  }
  printf("modules with taps %d, patched %d, left alone %d\n", seen, patched, kept);
  return 0;
}
