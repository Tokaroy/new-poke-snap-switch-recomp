// SPDX-License-Identifier: MIT
// Photo capture filter under render scale (see nvn_capture.h and analysis/core/photo_capture_quality.md).
//
// The game builds each photo with CaptureLanczos3PS (resource.drp): a 16-tap bilinear form of a 6x6 Lanczos3 whose tap spacing is
// 1/textureSize(SourceTexture). On the Switch the source is 1920x1080, so the 1080x720 photo is a 1.5x reduction. Here the
// translator's TXQ divides by support_buffer.render_scale, which is always 1.0, so on a 3840x2160 source (render scale 2) the
// kernel spans 6 host texels = 2 photo pixels: a 3x decimation with almost no prefilter, i.e. aliased grass and stair-stepped
// edges in the photo, its preview and the saved JPEG. The variant below runs the same Lanczos3 on the guest 1920x1080 grid and
// makes each guest texel the box average of its host footprint (4 bilinear taps 1/4 guest texel off center: exact 2x2 at scale 2,
// exact 4x4 at scale 4), so the photo matches the original filter on a supersampled image. Scale 1 keeps the game's shader.
// NVN_NO_CAPTURE_FIX=1 disables it (A/B).
#include "nvn_internal.h"
#include "nvn_capture.h"
#include "nvn_capture_spv.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

namespace nvn {
namespace {
// 0 = none of the game's capture resamplers, 1 = Lanczos3PS (plain), 2 = CaptureLanczos3PS (sRGB encode per tap, then * multiplyValue +
// addValue), 3 = CapturePS (one bilinear tap, sRGB encode, * multiplyValue + addValue: the 360x240 thumbnail pass)
int Classify(const StageProg& f) {
  if (!f.valid || f.tex.size() != 1 || f.tex[0].set != 2 || f.tex[0].binding != 32 || f.tex[0].array != 1 || f.spv.size() < 5) return 0;
  bool cb4 = false;
  for (const MCb& c : f.cb) if (c.slot == 4 && c.set == 0 && c.binding == 21) cb4 = true;   // CaptureCBuffer {addValue, multiplyValue}
  int query = 0, sinCos = 0, samples = 0, logExp = 0;
  uint32_t glslExt = 0xFFFFFFFFu;
  const std::vector<uint32_t>& w = f.spv;
  for (size_t i = 5; i < w.size();) {
    const uint32_t op = w[i] & 0xFFFF, n = w[i] >> 16;
    if (!n || i + n > w.size()) return 0;
    if (op == 11 && n >= 3 && !strcmp((const char*)&w[i + 2], "GLSL.std.450")) glslExt = w[i + 1];   // OpExtInstImport
    else if (op == 103) query++;                                                                     // OpImageQuerySizeLod
    else if (op == 87) samples++;                                                                    // OpImageSampleImplicitLod
    else if (op == 12 && n >= 5 && w[i + 3] == glslExt && (w[i + 4] == 13 || w[i + 4] == 14)) sinCos++;   // GLSL Sin / Cos
    else if (op == 12 && n >= 5 && w[i + 3] == glslExt && (w[i + 4] == 29 || w[i + 4] == 30)) logExp++;   // GLSL Exp2 / Log2
    i += n;
  }
  // Both shipped Lanczos3 resamplers: 2 size queries (x, y), 13 bilinear taps, sin of the 4 kernel phases and cos of 2.
  if (query >= 2 && samples == 13 && sinCos >= 4) return cb4 ? 2 : 1;
  // CapturePS: one tap, the sRGB curve (log2/exp2 per channel), no size query, no trig.
  if (cb4 && query == 0 && samples == 1 && sinCos == 0 && logExp >= 6) return 3;
  return 0;
}
const char* KindName(int k) { return k == 1 ? "Lanczos3PS" : k == 2 ? "CaptureLanczos3PS" : k == 3 ? "CapturePS" : "-"; }
// Sets the default value of the float spec constant with SpecId `id`.
bool PatchSpecFloat(std::vector<uint32_t>& w, uint32_t specId, float v) {
  uint32_t target = 0;
  for (size_t i = 5; i < w.size();) {
    const uint32_t op = w[i] & 0xFFFF, n = w[i] >> 16;
    if (!n) return false;
    if (op == 71 && n == 4 && w[i + 2] == 1 && w[i + 3] == specId) target = w[i + 1];   // OpDecorate %x SpecId id
    if (op == 50 && n == 4 && target && w[i + 2] == target) { memcpy(&w[i + 3], &v, 4); return true; }   // OpSpecConstant %float %x value
    i += n;
  }
  return false;
}
}  // namespace

int CaptureProgKind(ProgramImpl* p) {
  if (!p || !p->st[1].valid) return 0;
  static std::mutex mu;
  static std::map<ProgramImpl*, int> kind;
  std::lock_guard<std::mutex> lk(mu);
  auto k = kind.find(p);
  if (k == kind.end()) k = kind.emplace(p, Classify(p->st[1])).first;
  return k->second;
}

ProgramImpl* CaptureLanczosVariant(Host* h, ProgramImpl* p, const TextureImpl* src, const TextureImpl* rt0, size_t nTex) {
  static const bool off = getenv("NVN_NO_CAPTURE_FIX") != nullptr;
  static const bool dbg = getenv("NVN_CAPTURE_LOG") != nullptr;
  if (dbg && p && rt0) {
    static int n1080 = 0, n360 = 0;
    const bool is1080 = rt0->b.width == 1080 && rt0->b.height == 720, is360 = rt0->b.width == 360 && rt0->b.height == 240;
    if ((is1080 && n1080++ < 30) || (is360 && n360++ < 10)) {
      LogError("[capture] draw into %ux%u (host %ux%u) fs=%s ntex=%zu/%zu src=%ux%u host %ux%u kind=%d", rt0->b.width, rt0->b.height, rt0->HW(), rt0->HH(),
               p->st[1].valid ? p->st[1].key : "-", p->st[1].tex.size(), nTex, src ? src->b.width : 0u, src ? src->b.height : 0u, src ? src->HW() : 0u, src ? src->HH() : 0u,
               p->st[1].valid ? Classify(p->st[1]) : -1);
    }
  }
  if (off || !p || !src || !rt0 || nTex != 1 || !p->st[1].valid) return nullptr;
  const uint32_t gw = src->b.width, gh = src->b.height;
  if (!gw || !gh) return nullptr;
  const float sx = (float)src->HW() / (float)gw, sy = (float)src->HH() / (float)gh;
  const float tx = (float)rt0->HW() / std::max(1.0f, (float)rt0->b.width);
  if (sx <= tx * 1.01f && sy <= tx * 1.01f) return nullptr;   // source not scaled relative to the target: the game's own filter is right
  const int kind = CaptureProgKind(p);
  if (!kind) return nullptr;
  static std::mutex mu;
  static std::map<std::tuple<ProgramImpl*, uint32_t, uint32_t>, ProgramImpl*> vars;
  std::lock_guard<std::mutex> lk(mu);
  const auto key = std::make_tuple(p, gw, gh);
  auto it = vars.find(key);
  if (it != vars.end()) return it->second;
  ProgramImpl* v = nullptr;
  std::vector<uint32_t> words = kind == 2   ? std::vector<uint32_t>(k_caplz_srgb, k_caplz_srgb + sizeof k_caplz_srgb / 4)
                                : kind == 3 ? std::vector<uint32_t>(k_capone_srgb, k_capone_srgb + sizeof k_capone_srgb / 4)
                                                 : std::vector<uint32_t>(k_caplz_plain, k_caplz_plain + sizeof k_caplz_plain / 4);
  VkShaderModule mod = VK_NULL_HANDLE;
  if (PatchSpecFloat(words, 100, (float)gw) && PatchSpecFloat(words, 101, (float)gh)) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = words.size() * 4; ci.pCode = words.data();
    if (vkCreateShaderModule(h->device, &ci, nullptr, &mod) != VK_SUCCESS) mod = VK_NULL_HANDLE;
  }
  if (mod) {
    v = new ProgramImpl(*p);   // kept for the process lifetime (pipelines and the pipeline service refer to it), as EvalCopyDepthVariant
    v->composite = true;
    StageProg& f = v->st[1];
    snprintf(f.key, sizeof f.key, "cap%d_%ux%u", kind, gw, gh);
    snprintf(f.fkey, sizeof f.fkey, "capfilter%d_%ux%u", kind, gw, gh);
    f.mod = mod; f.modAlt = VK_NULL_HANDLE; f.spvAlt.clear(); f.fragMods.clear();
    f.outLocs = SpvFragmentOutputs(words); f.spv = std::move(words);
    LogError("photo capture: %s samples a %ux%u source at %.2fx into a %ux%u target; filtering on the guest grid (NVN_NO_CAPTURE_FIX=1 disables)",
             KindName(kind), gw, gh, sx, rt0->b.width, rt0->b.height);
  } else LogError("photo capture: %s variant module failed", KindName(kind));
  vars[key] = v;
  return v;
}
}  // namespace nvn
