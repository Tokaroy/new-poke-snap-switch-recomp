// SPDX-License-Identifier: MIT
// M3 (lighting plan section 4, docs/lighting-rt.md): RT sun shadows in the receivers. Every depth-compare tap of the DYNAMIC shadow map (texture
// control block C, the same test PatchShadowPcf uses) is replaced by ray queries toward the sun against the frame's TLAS (nvn_rt.cpp):
//
//   Rays are traced in SHADOW-MAP SPACE: the tap's own operands (u, v, depthRef) are the receiver's position in the map the caster pass rendered, and the TLAS
//   holds the captured caster triangles in that same space, so the ray starts at (u, v, depthRef - bias) and runs toward the light along -z (z = 0 is the light's
//   side). No receiver matrix, no world space and no sun vector are involved; the game's receiver coordinate frames (they are not world space) never matter.
//   vis    = 1 - (number of rays that hit) / N                      N = 1 (hard) or 2 / 4 / 8 (jittered cone, per-pixel noise + a frame seed)
//   result = inside the map's footprint (+ margin) ? vis : 1         outside it the original sampler's border value was 1 (lit) too
//
// Straight-line code only (no loops, no new blocks): all casters are opaque instances, so a single OpRayQueryProceedKHR walks the whole traversal and the
// committed intersection type says hit or miss. The tap's result id is kept (OpSelect), so every consumer and every Phi stays valid. Nothing is read from a
// descriptor: the TLAS device address and the constants come in one 128-byte push-constant block (FRAGMENT stage, set up by ProgramBuildLayout when RT
// shadows are configured, filled per receiver draw by nvn_draw.cpp):
//   0   mat4  unused (kept so the block layout stays put)
//   64  uvec2 TLAS device address        72  uvec2 unused
//   80  vec4  w = origin bias (depth units, subtracted from depthRef); xyz unused
//   96  vec4  w = lateral cone spread per unit of depth (map units): tan(half angle) * (map xy units per world unit) / (map depth units per world unit); 0 = hard shadow
//   112 vec4  x = 1 when a TLAS with casters exists this frame (0 = every receiver is lit), y = frame seed (0..63), z = footprint margin (map units)
// A moving sun only changes the caster matrix and the receivers' matrices (the game's own), so the sun stays a runtime parameter by construction.
// Valid under spirv-val --target-env vulkan1.3 on every receiver module (tests/nvn/shadowrt_test.sh).
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>
#include "nvn_internal.h"

namespace nvn {
namespace {
enum : uint32_t {
  kOpExtension = 10, kOpExtInstImport = 11, kOpExtInst = 12, kOpEntryPoint = 15, kOpCapability = 17, kOpTypeBool = 20, kOpTypeInt = 21, kOpTypeFloat = 22,
  kOpTypeVector = 23, kOpTypeMatrix = 24, kOpTypeStruct = 30, kOpTypePointer = 32, kOpConstant = 43, kOpConstantComposite = 44, kOpFunction = 54,
  kOpVariable = 59, kOpLoad = 61, kOpAccessChain = 65, kOpDecorate = 71, kOpMemberDecorate = 72, kOpVectorShuffle = 79, kOpCompositeConstruct = 80,
  kOpCompositeExtract = 81, kOpLabel = 248, kOpFAdd = 129, kOpFSub = 131, kOpFMul = 133, kOpFDiv = 136, kOpVectorTimesScalar = 142, kOpMatrixTimesVector = 145,
  kOpLogicalAnd = 167, kOpSelect = 169, kOpINotEqual = 171, kOpFOrdLessThanEqual = 188, kOpFOrdGreaterThanEqual = 190, kOpImageSampleDrefExplicitLod = 90,
  kOpTypeAccelerationStructureKHR = 5341, kOpTypeRayQueryKHR = 4472, kOpConvertUToAccelerationStructureKHR = 4447, kOpRayQueryInitializeKHR = 4473,
  kOpRayQueryProceedKHR = 4477, kOpRayQueryGetIntersectionTypeKHR = 4479, kCapRayQueryKHR = 4472,
  kDecBlock = 2, kDecColMajor = 5, kDecMatrixStride = 7, kDecBuiltIn = 11, kDecOffset = 35, kBuiltInFragCoord = 15,
  kScFunction = 7, kScPushConstant = 9, kScInput = 1,
  kGlslFract = 10, kGlslCos = 14, kGlslSin = 13, kGlslSqrt = 31, kGlslCross = 68, kGlslNormalize = 69, kGlslFMax = 40,
};
void Emit(std::vector<uint32_t>& v, uint32_t op, std::initializer_list<uint32_t> w) {
  v.push_back(((uint32_t)(w.size() + 1) << 16) | op);
  v.insert(v.end(), w.begin(), w.end());
}
uint32_t FBits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
}  // namespace

// rays: 1 (hard shadow), 2, 4 or 8 (soft: jittered cone). Returns false (out untouched) when the module has no dynamic-map tap it can replace.
bool PatchShadowRt(const std::vector<uint32_t>& in, int rays, std::vector<uint32_t>& out) {
  if (in.size() < 6 || in[0] != 0x07230203 || rays < 1 || rays > 16) return false;
  std::map<uint32_t, bool> isF32;
  std::map<uint32_t, uint32_t> vec2Of;
  std::map<uint32_t, uint32_t> resType;
  std::map<uint32_t, std::string> nameOf;
  std::map<uint32_t, uint32_t> loadPtr, sampImgOf;
  std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, uint32_t> typeId;   // (op, a, b, c) -> id for int/float/bool/vector/matrix/pointer types
  size_t firstFn = 0, capEnd = 0, annotAt = 0, epAt = 0, epWc = 0;
  uint32_t glslSet = 0, fragCoordVar = 0, entryFn = 0;
  struct Tap { size_t at; size_t fnLabelEnd; };
  std::vector<Tap> taps;
  size_t curLabelEnd = 0; bool wantLabel = false;
  for (size_t i = 5; i < in.size();) {
    const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
    if (!wc || i + wc > in.size()) return false;
    if (op == kOpCapability) { capEnd = i + wc; if (in[i + 1] == kCapRayQueryKHR) return false; }
    else if (op == kOpExtInstImport && wc >= 3 && !strncmp((const char*)&in[i + 2], "GLSL.std.450", 12)) glslSet = in[i + 1];
    else if (op == kOpEntryPoint && !epAt) { epAt = i; epWc = wc; if (in[i + 1] == 4) entryFn = in[i + 2]; }
    else if (op == 5 && wc >= 3) nameOf[in[i + 1]] = std::string((const char*)&in[i + 2], strnlen((const char*)&in[i + 2], (wc - 2) * 4));
    else if (op == kOpDecorate && wc >= 4 && in[i + 2] == kDecBuiltIn && in[i + 3] == kBuiltInFragCoord) fragCoordVar = in[i + 1];
    else if (op == 61 && wc >= 4) loadPtr[in[i + 2]] = in[i + 3];
    else if (op == 86 && wc >= 5) sampImgOf[in[i + 2]] = in[i + 3];
    if (!annotAt && op >= kOpTypeBool && op <= 39 && op != 21 + 100) annotAt = i;   // first type declaration: annotations end before it
    if (op == kOpTypeInt && wc >= 4) typeId[{op, in[i + 2], in[i + 3], 0}] = in[i + 1];
    else if (op == kOpTypeFloat && wc >= 3) { typeId[{op, in[i + 2], 0, 0}] = in[i + 1]; if (in[i + 2] == 32) isF32[in[i + 1]] = true; }
    else if (op == kOpTypeBool) typeId[{op, 0, 0, 0}] = in[i + 1];
    else if (op == kOpTypeVector && wc >= 4) { typeId[{op, in[i + 2], in[i + 3], 0}] = in[i + 1]; if (in[i + 3] == 2) vec2Of[in[i + 1]] = in[i + 2]; }
    else if (op == kOpTypeMatrix && wc >= 4) typeId[{op, in[i + 2], in[i + 3], 0}] = in[i + 1];
    else if (op == kOpTypePointer && wc >= 4) typeId[{op, in[i + 2], in[i + 3], 0}] = in[i + 1];
    if (op == kOpFunction) { if (!firstFn) firstFn = i; wantLabel = true; }
    else if (op == kOpLabel && wantLabel) { curLabelEnd = i + wc; wantLabel = false; }
    else if (firstFn && wc >= 3) resType[in[i + 2]] = in[i + 1];
    if (op == kOpImageSampleDrefExplicitLod && wc >= 8 && firstFn) taps.push_back({i, curLabelEnd});
    i += wc;
  }
  if (!firstFn || taps.empty() || !capEnd || !annotAt || !epAt || in[epAt + 1] != 4) return false;   // fragment programs only (a few vertex programs sample the map too: they keep it)
  auto isDyn = [&](uint32_t img) {   // texture control block C = the dynamic shadow map (the same test as PatchShadowPcf)
    uint32_t id = img;
    auto si = sampImgOf.find(id); if (si != sampImgOf.end()) id = si->second;
    auto lp = loadPtr.find(id); if (lp != loadPtr.end()) id = lp->second;
    auto n = nameOf.find(id);
    return n != nameOf.end() && n->second.size() > 2 && n->second.compare(n->second.size() - 2, 2, "_C") == 0;
  };
  std::vector<size_t> use;   // indices into taps
  uint32_t fT = 0, v2T = 0;
  for (size_t t = 0; t < taps.size(); t++) {
    const size_t i = taps[t].at;
    const uint32_t resT = in[i + 1], coord = in[i + 4];
    auto ct = resType.find(coord);
    if (!isF32.count(resT) || ct == resType.end() || !vec2Of.count(ct->second) || vec2Of[ct->second] != resT) continue;
    if (in[i + 6] != 2) continue;
    if (!isDyn(in[i + 3])) continue;
    if (fT && (fT != resT || v2T != ct->second)) continue;   // one float and vec2 type per module (always true in practice)
    fT = resT; v2T = ct->second; use.push_back(t);
  }
  if (use.empty()) return false;

  uint32_t next = in[3];
  std::vector<uint32_t> capB, annB, typB, fnVarsDummy;
  // ---- types (looked up first: duplicate non-aggregate types are invalid) ----
  auto getType = [&](uint32_t op, uint32_t a, uint32_t b, uint32_t c, std::initializer_list<uint32_t> words) {
    auto it = typeId.find({op, a, b, c});
    if (it != typeId.end()) return it->second;
    const uint32_t id = next++;
    std::vector<uint32_t> w; w.push_back(id); w.insert(w.end(), words.begin(), words.end());
    typB.push_back(((uint32_t)(w.size() + 1) << 16) | op); typB.insert(typB.end(), w.begin(), w.end());
    typeId[{op, a, b, c}] = id;
    return id;
  };
  const uint32_t boolT = getType(kOpTypeBool, 0, 0, 0, {});
  const uint32_t uT = getType(kOpTypeInt, 32, 0, 0, {32, 0});
  const uint32_t v2uT = getType(kOpTypeVector, uT, 2, 0, {uT, 2});
  const uint32_t v3T = getType(kOpTypeVector, fT, 3, 0, {fT, 3});
  const uint32_t v4T = getType(kOpTypeVector, fT, 4, 0, {fT, 4});
  const uint32_t m4T = getType(kOpTypeMatrix, v4T, 4, 0, {v4T, 4});
  uint32_t accT = 0, rqT = 0;
  { accT = next++; Emit(typB, kOpTypeAccelerationStructureKHR, {accT}); rqT = next++; Emit(typB, kOpTypeRayQueryKHR, {rqT}); }
  const uint32_t rqPtrT = next++; Emit(typB, kOpTypePointer, {rqPtrT, kScFunction, rqT});
  // push constant block
  const uint32_t pcT = next++; Emit(typB, kOpTypeStruct, {pcT, m4T, v2uT, v2uT, v4T, v4T, v4T});
  Emit(annB, kOpDecorate, {pcT, kDecBlock});
  const uint32_t offs[6] = {0, 64, 72, 80, 96, 112};
  for (uint32_t m = 0; m < 6; m++) Emit(annB, kOpMemberDecorate, {pcT, m, kDecOffset, offs[m]});
  Emit(annB, kOpMemberDecorate, {pcT, 0, kDecColMajor});
  Emit(annB, kOpMemberDecorate, {pcT, 0, kDecMatrixStride, 16});
  const uint32_t pcPtrT = next++; Emit(typB, kOpTypePointer, {pcPtrT, kScPushConstant, pcT});
  const uint32_t pcM4PtrT = getType(kOpTypePointer, kScPushConstant, m4T, 0, {kScPushConstant, m4T});
  const uint32_t pcV2uPtrT = getType(kOpTypePointer, kScPushConstant, v2uT, 0, {kScPushConstant, v2uT});
  const uint32_t pcV4PtrT = getType(kOpTypePointer, kScPushConstant, v4T, 0, {kScPushConstant, v4T});
  const uint32_t pcVar = next++; Emit(typB, kOpVariable, {pcPtrT, pcVar, kScPushConstant});
  bool addedFragCoord = false;
  const uint32_t inV4PtrT = getType(kOpTypePointer, kScInput, v4T, 0, {kScInput, v4T});
  if (!fragCoordVar) {
    fragCoordVar = next++; addedFragCoord = true;
    Emit(typB, kOpVariable, {inV4PtrT, fragCoordVar, kScInput});
    Emit(annB, kOpDecorate, {fragCoordVar, kDecBuiltIn, kBuiltInFragCoord});
  }
  // ---- constants ----
  std::map<uint32_t, uint32_t> fconst, uconst;
  auto fc = [&](float f) { auto it = fconst.find(FBits(f)); if (it != fconst.end()) return it->second; const uint32_t id = next++; Emit(typB, kOpConstant, {fT, id, FBits(f)}); fconst[FBits(f)] = id; return id; };
  auto uc = [&](uint32_t u) { auto it = uconst.find(u); if (it != uconst.end()) return it->second; const uint32_t id = next++; Emit(typB, kOpConstant, {uT, id, u}); uconst[u] = id; return id; };
  const uint32_t f0 = fc(0.0f), f1 = fc(1.0f);
  const uint32_t u0 = uc(0), u1 = uc(1), uMask = uc(0xFF), uFlags = uc(0x5);   // Opaque | TerminateOnFirstHit
  const uint32_t c0 = uc(0), c1 = uc(1), c2 = uc(2), c3 = uc(3), c4 = uc(4), c5 = uc(5);   // push-constant member indices (int would do; uint is fine for OpAccessChain)
  (void)c0;
  const float kGold = 0.61803399f, kTwoPi = 6.28318531f;
  const uint32_t fIgnA = fc(52.9829189f), fIgnB = fc(0.06711056f), fIgnC = fc(0.00583715f), fSeedStep = fc(5.588238f), fTwoPi = fc(kTwoPi);
  uint32_t glsl = glslSet; std::vector<uint32_t> impB;
  if (!glsl) {
    glsl = next++;
    const char* nm = "GLSL.std.450"; uint32_t buf[4] = {0, 0, 0, 0}; memcpy(buf, nm, 12);
    impB.push_back(((uint32_t)(1 + 1 + 4) << 16) | kOpExtInstImport); impB.push_back(glsl); impB.insert(impB.end(), buf, buf + 4);
  }
  // every constant the expansion needs, created now: typB is spliced into the output once, before the first function
  std::vector<uint32_t> cks, crs;
  for (int k = 0; k < rays; k++) { cks.push_back(fc((float)k * kGold)); crs.push_back(fc(std::sqrt(((float)k + 0.5f) / (float)rays))); }
  const uint32_t fInvRays = fc(1.0f / (float)rays);
  // ray-query variable per function that contains a replaced tap
  std::map<size_t, uint32_t> rqVarOfFn;   // fnLabelEnd -> variable id
  for (size_t t : use) { const size_t k = taps[t].fnLabelEnd; if (!rqVarOfFn.count(k)) rqVarOfFn[k] = next++; }

  // ---- assemble ----
  // order of insertion points: capabilities+extension (capEnd) < ext import (before OpMemoryModel; or first existing import) < annotations (annotAt) < types/vars (before firstFn)
  size_t memModelAt = 0;
  for (size_t i = 5; i < in.size();) { const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF; if (op == 14) { memModelAt = i; break; } i += wc; }
  if (!memModelAt) return false;
  out.assign(in.begin(), in.begin() + 5);
  size_t ui = 0;
  std::map<size_t, bool> labelDone;
  for (size_t i = 5; i < in.size();) {
    const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
    if (i == capEnd - 0 && false) {}
    // capability / extension go right after the last capability: handled when we pass capEnd (the instruction at index capEnd is the first non-capability)
    if (i == capEnd) {
      Emit(out, kOpCapability, {kCapRayQueryKHR});
      const char* ext = "SPV_KHR_ray_query"; uint32_t buf[5] = {0, 0, 0, 0, 0}; memcpy(buf, ext, strlen(ext));
      out.push_back(((uint32_t)(1 + 5) << 16) | kOpExtension); out.insert(out.end(), buf, buf + 5);
    }
    if (i == memModelAt && !impB.empty()) out.insert(out.end(), impB.begin(), impB.end());
    if (i == annotAt) out.insert(out.end(), annB.begin(), annB.end());
    if (i == firstFn) out.insert(out.end(), typB.begin(), typB.end());
    if (i == epAt && addedFragCoord) {   // the new Input variable joins the entry point's interface
      std::vector<uint32_t> ep(in.begin() + i, in.begin() + i + wc);
      ep.push_back(fragCoordVar);
      ep[0] = ((uint32_t)ep.size() << 16) | kOpEntryPoint;
      out.insert(out.end(), ep.begin(), ep.end());
      i += wc;
      continue;
    }
    if (op == kOpImageSampleDrefExplicitLod && ui < use.size() && taps[use[ui]].at == i) {
      const uint32_t res = in[i + 2], coord = in[i + 4], dref = in[i + 5];
      auto id = [&]() { return next++; };
      // the tap's operands are the receiver's position in the shadow map
      const uint32_t cu = id(), cv = id();
      Emit(out, kOpCompositeExtract, {fT, cu, coord, 0});
      Emit(out, kOpCompositeExtract, {fT, cv, coord, 1});
      // push constants: tlas, bias, cone, prm
      const uint32_t pA = id(), a2 = id(), accel = id();
      Emit(out, kOpAccessChain, {pcV2uPtrT, pA, pcVar, c1});
      Emit(out, kOpLoad, {v2uT, a2, pA});
      Emit(out, kOpConvertUToAccelerationStructureKHR, {accT, accel, a2});
      const uint32_t pL = id(), L4 = id(), pT = id(), T4 = id(), pP = id(), P4 = id();
      Emit(out, kOpAccessChain, {pcV4PtrT, pL, pcVar, c3}); Emit(out, kOpLoad, {v4T, L4, pL});
      Emit(out, kOpAccessChain, {pcV4PtrT, pT, pcVar, c4}); Emit(out, kOpLoad, {v4T, T4, pT});
      Emit(out, kOpAccessChain, {pcV4PtrT, pP, pcVar, c5}); Emit(out, kOpLoad, {v4T, P4, pP});
      (void)c0; (void)c2; (void)pcM4PtrT; (void)m4T;
      const uint32_t bias = id(), cone = id(), enable = id(), seed = id();
      Emit(out, kOpCompositeExtract, {fT, bias, L4, 3});
      Emit(out, kOpCompositeExtract, {fT, cone, T4, 3});
      Emit(out, kOpCompositeExtract, {fT, enable, P4, 0});
      Emit(out, kOpCompositeExtract, {fT, seed, P4, 1});
      // origin = (u, v, depthRef - bias); the ray runs along -z for that long
      const uint32_t dz = id(), zr = id(), org = id(), negOne = id();
      Emit(out, kOpFSub, {fT, dz, dref, bias});
      Emit(out, kOpExtInst, {fT, zr, glsl, kGlslFMax, dz, f0});
      Emit(out, kOpCompositeConstruct, {v3T, org, cu, cv, zr});
      Emit(out, kOpFSub, {fT, negOne, f0, f1});
      // inside the map: 0 <= u, v <= 1 (+ margin) and the frame has casters
      uint32_t inside = 0;
      auto cmp = [&](uint32_t cmpOp, uint32_t a, uint32_t b) { const uint32_t r = id(); Emit(out, cmpOp, {boolT, r, a, b}); return r; };
      auto andd = [&](uint32_t a, uint32_t b) { if (!a) return b; const uint32_t r = id(); Emit(out, kOpLogicalAnd, {boolT, r, a, b}); return r; };
      const uint32_t ext = id(), lo = id(), hi = id();
      Emit(out, kOpCompositeExtract, {fT, ext, P4, 2});
      Emit(out, kOpFSub, {fT, lo, f0, ext});
      Emit(out, kOpFAdd, {fT, hi, f1, ext});
      for (uint32_t v : {cu, cv}) { inside = andd(inside, cmp(kOpFOrdGreaterThanEqual, v, lo)); inside = andd(inside, cmp(kOpFOrdLessThanEqual, v, hi)); }   // the map's footprint (+ the margin)
      inside = andd(inside, cmp(kOpFOrdGreaterThanEqual, enable, f1));
      const uint32_t tmax = id();
      Emit(out, kOpSelect, {fT, tmax, inside, zr, f0});   // outside the map or no casters: a zero-length ray never hits
      // noise (interleaved gradient noise of the pixel, shifted by the frame seed)
      uint32_t noise = f0;
      if (rays > 1) {
        const uint32_t fcv = id(), fx = id(), fy = id(), sx = id(), x1 = id(), y1 = id(), a1 = id(), a2n = id(), fa = id(), a3 = id(), a4 = id(), a5 = id();
        Emit(out, kOpLoad, {v4T, fcv, fragCoordVar});
        Emit(out, kOpCompositeExtract, {fT, fx, fcv, 0});
        Emit(out, kOpCompositeExtract, {fT, fy, fcv, 1});
        Emit(out, kOpFMul, {fT, sx, seed, fSeedStep});
        Emit(out, kOpFAdd, {fT, x1, fx, sx});
        Emit(out, kOpFAdd, {fT, y1, fy, sx});
        Emit(out, kOpFMul, {fT, a1, x1, fIgnB});
        Emit(out, kOpFMul, {fT, a2n, y1, fIgnC});
        Emit(out, kOpFAdd, {fT, fa, a1, a2n});
        Emit(out, kOpExtInst, {fT, a3, glsl, kGlslFract, fa});
        Emit(out, kOpFMul, {fT, a4, a3, fIgnA});
        Emit(out, kOpExtInst, {fT, a5, glsl, kGlslFract, a4});
        noise = a5;
      }
      const uint32_t rqVar = rqVarOfFn[taps[use[ui]].fnLabelEnd];
      uint32_t sum = 0;
      for (int k = 0; k < rays; k++) {
        uint32_t dir = 0;
        {
          const uint32_t d0 = id();
          if (rays > 1) {
            const uint32_t ck = cks[k], cr = crs[k];
            const uint32_t s0 = id(), s1 = id(), ang = id(), cs = id(), sn = id(), rc = id(), rs = id(), rr = id();
            Emit(out, kOpFAdd, {fT, s0, noise, ck});
            Emit(out, kOpExtInst, {fT, s1, glsl, kGlslFract, s0});
            Emit(out, kOpFMul, {fT, ang, s1, fTwoPi});
            Emit(out, kOpExtInst, {fT, cs, glsl, kGlslCos, ang});
            Emit(out, kOpExtInst, {fT, sn, glsl, kGlslSin, ang});
            Emit(out, kOpFMul, {fT, rr, cone, cr});
            Emit(out, kOpFMul, {fT, rc, rr, cs});
            Emit(out, kOpFMul, {fT, rs, rr, sn});
            Emit(out, kOpCompositeConstruct, {v3T, d0, rc, rs, negOne});   // |z| is the ray parameter, so the direction needs no normalization
          } else Emit(out, kOpCompositeConstruct, {v3T, d0, f0, f0, negOne});
          dir = d0;
        }
        const uint32_t pr = id(), ty = id(), nz = id(), hitF = id();
        (void)pr;
        Emit(out, kOpRayQueryInitializeKHR, {rqVar, accel, uFlags, uMask, org, f0, dir, tmax});
        Emit(out, kOpRayQueryProceedKHR, {boolT, pr, rqVar});
        Emit(out, kOpRayQueryGetIntersectionTypeKHR, {uT, ty, rqVar, u1});
        Emit(out, kOpINotEqual, {boolT, nz, ty, u0});
        Emit(out, kOpSelect, {fT, hitF, nz, f1, f0});
        if (!sum) sum = hitF; else { const uint32_t s = id(); Emit(out, kOpFAdd, {fT, s, sum, hitF}); sum = s; }
      }
      const uint32_t frac = id(), vis = id();
      Emit(out, kOpFMul, {fT, frac, sum, fInvRays});
      Emit(out, kOpFSub, {fT, vis, f1, frac});
      Emit(out, kOpSelect, {fT, res, inside, vis, f1});
      ui++;
    } else out.insert(out.end(), in.begin() + i, in.begin() + i + wc);
    // the ray-query variable of a function goes first in its first block
    if (op == kOpLabel) { auto it = rqVarOfFn.find(i + wc); if (it != rqVarOfFn.end()) Emit(out, kOpVariable, {rqPtrT, it->second, kScFunction}); }
    i += wc;
  }
  (void)entryFn; (void)uMask; (void)labelDone;
  out[3] = next;
  return true;
}
}  // namespace nvn
