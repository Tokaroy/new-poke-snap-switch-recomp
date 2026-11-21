// SPDX-License-Identifier: MIT
// Offline check of the alpha-test wrappers (runtime/nvn/nvn_foliage.cpp): for every translated fragment module with a vec4 output at location 0,
// PatchCaptureAlphaLod (Foliage stability) then PatchAlphaHash in three forms (stability only, stability + hashed edge, hashed edge only), and writes
// each patched module so spirv-val can check it. usage: foliage_test <cache dir with *.spv> <out dir>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include "nvn_foliage.h"
#include "nvn_internal.h"
namespace nvn {
int g_presentCount = 0;
// link stubs: the parts of nvn_foliage.cpp this test does not run
bool FxIsPhotoFrame() { return false; }
void FxNoteRan() {}
int MotionAaProbeState() { return -1; }
bool FoliageStableEnabled() { return true; }
}  // namespace nvn
extern "C" float snap_route_ride_progress() { return 0.0f; }
bool dev_env_on() { return false; }
void dev_param_f_impl(const char*, float*, float, float, float) {}
void dev_param_i_impl(const char*, int*, int, int, int) {}
namespace fs = std::filesystem;
static void Write(const fs::path& p, const std::vector<uint32_t>& w) { std::ofstream o(p, std::ios::binary); o.write((const char*)w.data(), w.size() * 4); }
int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: foliage_test <cache dir> <out dir>\n"); return 2; }
  fs::create_directories(argv[2]);
  int seen = 0, lod = 0, cov = 0, both = 0, hash = 0, notFirst = 0;
  for (auto& e : fs::directory_iterator(argv[1])) {
    if (e.path().extension() != ".spv") continue;
    std::ifstream f(e.path(), std::ios::binary); std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
    if (b.size() < 24 || b.size() % 4) continue;
    std::vector<uint32_t> w(b.size() / 4); memcpy(w.data(), b.data(), b.size());
    bool frag = false;
    for (size_t i = 5; i < w.size();) { const uint32_t wc = w[i] >> 16; if (!wc) break; if ((w[i] & 0xFFFF) == 15 && w[i + 1] == 4) frag = true; i += wc; }
    if (!frag) continue;
    seen++;
    std::vector<uint32_t> cap, out;
    uint32_t var = 0;
    const std::string stem = e.path().stem().string();
    if (nvn::PatchCaptureAlphaLod(w, cap, &var)) {
      lod++;
      {   // was the level taken after a later sample (the one whose alpha the shader reads) rather than the first?
        int n = 0, at = -1;
        for (size_t i = 5; i < cap.size();) { const uint32_t wc = cap[i] >> 16; if (!wc) break; const uint32_t op = cap[i] & 0xFFFF;
          if (op == 87) n++; if (op == 105 && at < 0) at = n; i += wc; }
        if (at > 1) notFirst++;
      }
      if (nvn::PatchAlphaHash(cap, 5, false, false, 0, out, var)) { cov++; Write(fs::path(argv[2]) / (stem + "_cov.spv"), out); }
      if (nvn::PatchAlphaHash(cap, 5, false, true, 0, out, var)) { both++; Write(fs::path(argv[2]) / (stem + "_covhash.spv"), out); }
    }
    if (nvn::PatchAlphaHash(w, 5, false, true, 0, out)) { hash++; Write(fs::path(argv[2]) / (stem + "_hash.spv"), out); }
  }
  printf("fragment modules %d, lod captured %d (%d on the alpha sample after the first), stability %d, stability + edge %d, edge only %d\n", seen, lod, notFirst, cov, both, hash);
  return 0;
}
