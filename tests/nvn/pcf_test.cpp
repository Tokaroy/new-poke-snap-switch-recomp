// SPDX-License-Identifier: MIT
// Offline check of PatchShadowPcf (runtime/nvn/nvn_shadowpcf.cpp): patches every translated fragment program that has depth-compare taps and writes the
// patched modules so spirv-val can check them. usage: pcf_test <cache dir with *.spv> <out dir> <taps> <pcss 0|1> [remapK]
// then: python -I tests/nvn/pcf_validate.py <out dir>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#include "nvn_internal.h"
namespace nvn { int g_presentCount = 0; }
namespace fs = std::filesystem;
int main(int argc, char** argv) {
  if (argc < 5) { fprintf(stderr, "usage: pcf_test <cache dir> <out dir> <taps> <pcss 0|1> [remapK]\n"); return 2; }
  const int taps = atoi(argv[3]); const bool pcss = atoi(argv[4]) != 0; const float k = argc > 5 ? (float)atof(argv[5]) : 1.0f;
  fs::create_directories(argv[2]);
  nvn::PcssParams pp;
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
    if (nvn::PatchShadowPcf(w, taps, 1.5f / 1024.0f, 0.0f, k, out, pcss ? &pp : nullptr)) {
      patched++;
      std::ofstream o(fs::path(argv[2]) / e.path().filename(), std::ios::binary); o.write((const char*)out.data(), out.size() * 4);
    } else kept++;
  }
  printf("modules with taps %d, patched %d, left alone %d\n", seen, patched, kept);
  return 0;
}
