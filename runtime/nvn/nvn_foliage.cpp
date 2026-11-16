// SPDX-License-Identifier: MIT
// See nvn_foliage.h.
#include "nvn_foliage.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "nvn_dev.h"
#include "nvn_fx.h"

namespace nvn {

namespace {
bool EnvOn(const char* n) { const char* e = getenv(n); return e && *e && strcmp(e, "0") != 0 && _stricmp(e, "off") != 0; }
// Live values (dev loop: the A/B capture tool and F1 "Tuning" set them by env name); without SNAP_DEV they are the env values read once.
struct Live {
  int aa = EnvOn("SNAP_FOLIAGE_AA") ? 1 : 0;
  float leaf = [] { const char* e = getenv("SNAP_LEAF_TRANSLUCENCY"); if (!e || !*e || _stricmp(e, "off") == 0) return 0.0f; const float f = (float)atof(e); return f < 0 ? 0.0f : f > 1 ? 1.0f : f; }();
  int soft = EnvOn("SNAP_SOFT_PARTICLES") ? 1 : 0;
  int leafFlip = EnvOn("SNAP_LEAF_FLIP") ? 1 : 0;      // debug: negate the camera-toward-light dot product
  int leafDebug = EnvOn("SNAP_LEAF_DEBUG") ? 1 : 0;    // debug: leaf term painted pure red (shows where it applies)
  Live() {
    dev_param_i("SNAP_LEAF_FLIP", &leafFlip, 0, 1, 0);
    dev_param_i("SNAP_LEAF_DEBUG", &leafDebug, 0, 1, 0);
    dev_param_i("SNAP_FOLIAGE_AA", &aa, 0, 1, 0);
    dev_param_f("SNAP_LEAF_TRANSLUCENCY", &leaf, 0.0f, 1.0f, 0.0f);
    dev_param_i("SNAP_SOFT_PARTICLES", &soft, 0, 1, 0);
    if (aa || leaf > 0 || soft) fprintf(stderr, "[foliage] on: leaf edge smoothing %d, leaf translucency %.2f, soft particles %d (photo frames keep the game's draw)\n", aa, leaf, soft);
  }
};
Live& L() { static Live l; return l; }
}  // namespace
bool FoliageAaEnabled() { return L().aa != 0; }
float FoliageLeafStrength() { const float f = L().leaf; return f < 0 ? 0.0f : f > 1 ? 1.0f : f; }
bool SoftParticlesEnabled() { return L().soft != 0; }
int FoliageLeafDebugFlags() { return (L().leafFlip ? 16 : 0) | (L().leafDebug ? 32 : 0); }

// ---- hashed alpha / alpha to coverage (SPIR-V wrapper, like PatchAlphaTest in nvn_alpha.cpp) --------------------------------
namespace {
enum : uint32_t {
  kOpEntryPoint = 15, kOpExecutionMode = 16, kOpTypeBool = 20, kOpTypeFloat = 22, kOpTypeVector = 23, kOpTypePointer = 32, kOpVariable = 59,
  kOpFunction = 54, kOpFunctionCall = 57, kOpLoad = 61, kOpStore = 62, kOpLabel = 248, kOpBranchConditional = 250, kOpReturn = 253, kOpKill = 252,
  kOpFunctionEnd = 56, kOpSelectionMerge = 247, kOpDecorate = 71, kOpMemberDecorate = 72, kOpDecorationGroup = 73, kOpGroupDecorate = 74,
  kOpGroupMemberDecorate = 75, kOpSpecConstant = 50, kOpConstant = 43, kOpCompositeExtract = 81, kOpCompositeInsert = 82, kOpFAdd = 129,
  kOpFSub = 131, kOpFMul = 133, kOpFDiv = 136, kOpFRem = 140, kOpFOrdGreaterThan = 186, kOpFwidth = 209,
};
void Emit(std::vector<uint32_t>& v, uint32_t op, std::initializer_list<uint32_t> w) {
  v.push_back(((uint32_t)(w.size() + 1) << 16) | op);
  v.insert(v.end(), w.begin(), w.end());
}
uint32_t FBits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
}  // namespace

// func: NVN compare enum (2 LESS, 4 LEQUAL, 5 GREATER, 7 GEQUAL). hash: the wrapper keeps the contour of the game's test (alpha == ref) but turns the hard
// edge into a one pixel wide ramp: s = (a - ref) / (0.5 * fwidth(a) + eps) + 0.5, then a fragment survives when s beats a per-pixel interleaved-gradient
// noise value (dither; SSAA, SMAA and the temporal upscalers average it into an anti-aliased edge). Flat alpha (fwidth ~ 0) keeps the game's
// exact decision. a2c: no discard, s is written to the alpha output and the pipeline enables alphaToCoverage (multisampled targets only).
// !hash: the game's own hard test (same compare as PatchAlphaTest), used when only the leaf term is on.
// leaf 1..8: back-lit gain. The camera direction (PerViewCBuffer, set 0 binding 20, vec4 37) and the light (set 0 binding 22, vec4 1 = direction the light travels, negated to get toward-light)
// give d = dot(forward, toLight); looking toward the sun (d > -0.35) the lit colour is multiplied by 1 + gain * ((d+0.35)/1.35)^2 * tint (warm: red kept,
// green 0.9, blue 0.35), as sunlight showing through the leaf. Modules without those two constant buffers (or the GLSL.std450 import) get no leaf term.
// Returns false when the module is not of the expected shape (the caller then keeps the game's own hard test).
bool PatchAlphaHash(const std::vector<uint32_t>& in, int func, bool a2c, bool hash, int leaf, std::vector<uint32_t>& out) {
  if (func != 2 && func != 4 && func != 5 && func != 7) return false;
  if (in.size() < 6 || in[0] != 0x07230203) return false;
  const uint32_t bound = in[3];
  uint32_t entry = 0, var0 = 0, voidT = 0, fnT = 0, boolT = 0, annotEnd = 0, firstFn = 0, fcVar = 0, inPtr = 0, varType = 0, glslSet = 0, intT = 0, uPtrF = 0;
  struct Ptr { uint32_t id, sc, pointee; };
  std::vector<Ptr> ptrs;
  struct Vec { uint32_t id, comp, n; };
  std::vector<Vec> vecs;
  std::vector<uint32_t> floats;
  std::vector<std::pair<uint32_t, uint32_t>> varSc;           // variable id -> pointer type
  std::vector<std::pair<uint32_t, uint32_t>> bindOf, setOf;   // variable id -> Binding / DescriptorSet
  size_t i = 5;
  while (i < in.size()) {
    const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
    if (!wc || i + wc > in.size()) return false;
    switch (op) {
      case kOpEntryPoint: if (in[i + 1] == 4 && !entry) entry = in[i + 2]; break;
      case 11: if (wc >= 4 && !strncmp((const char*)&in[i + 2], "GLSL.std.450", 12)) glslSet = in[i + 1]; break;   // OpExtInstImport
      case kOpDecorate:
        if (wc >= 4 && in[i + 2] == 30 && in[i + 3] == 0) var0 = in[i + 1];
        if (wc >= 4 && in[i + 2] == 11 && in[i + 3] == 15) fcVar = in[i + 1];   // BuiltIn FragCoord
        if (wc >= 4 && in[i + 2] == 33) bindOf.push_back({in[i + 1], in[i + 3]});
        if (wc >= 4 && in[i + 2] == 34) setOf.push_back({in[i + 1], in[i + 3]});
        annotEnd = (uint32_t)(i + wc); break;
      case kOpMemberDecorate: case kOpDecorationGroup: case kOpGroupDecorate: case kOpGroupMemberDecorate: annotEnd = (uint32_t)(i + wc); break;
      case kOpTypeBool: boolT = in[i + 1]; break;
      case 21: if (wc >= 4 && in[i + 2] == 32 && !intT) intT = in[i + 1]; break;   // OpTypeInt 32
      case kOpTypeFloat: if (in[i + 2] == 32) floats.push_back(in[i + 1]); break;
      case kOpTypeVector: vecs.push_back({in[i + 1], in[i + 2], in[i + 3]}); break;
      case kOpTypePointer: ptrs.push_back({in[i + 1], in[i + 2], in[i + 3]}); break;
      case kOpVariable: varSc.push_back({in[i + 2], in[i + 1]}); break;
      case kOpFunction: if (!firstFn) firstFn = (uint32_t)i; if (in[i + 2] == entry) { fnT = in[i + 4]; voidT = in[i + 1]; } break;
      default: break;
    }
    i += wc;
  }
  if (!entry || !var0 || !fnT || !firstFn || !annotEnd) return false;
  for (auto& v : varSc) if (v.first == var0) varType = v.second;
  uint32_t vecT = 0, floatT = 0;
  for (auto& p : ptrs) if (p.id == varType) vecT = p.pointee;
  for (auto& v : vecs) if (v.id == vecT && v.n == 4) for (uint32_t f : floats) if (f == v.comp) floatT = f;
  if (!floatT) return false;
  const bool dither = hash && !a2c;
  const bool addFc = dither && !fcVar;
  if (dither && fcVar) {   // an existing FragCoord must be an Input vec4 float; reuse it
    bool ok = false;
    for (auto& v : varSc) if (v.first == fcVar) for (auto& p : ptrs) if (p.id == v.second && p.sc == 1 && p.pointee == vecT) ok = true;
    if (!ok) return false;
  }
  for (auto& p : ptrs) if (p.sc == 1 && p.pointee == vecT) inPtr = p.id;
  for (auto& p : ptrs) if (p.sc == 2 && p.pointee == floatT) uPtrF = p.id;
  auto lookup = [](const std::vector<std::pair<uint32_t, uint32_t>>& m, uint32_t v, uint32_t& val) { for (auto& e : m) if (e.first == v) { val = e.second; return true; } return false; };
  uint32_t cam = 0, light = 0;   // set 0 binding 20 / 22 uniform variables
  for (auto& b : bindOf) {
    uint32_t st = 0;
    if (!lookup(setOf, b.first, st) || st != 0) continue;
    if (b.second == 20) cam = b.first; else if (b.second == 22) light = b.first;
  }
  const bool leafFlip = (leaf & 16) != 0;
  const int leafLvl = leaf & 15;   // 1..8 gain, 9 = debug (red)
  const bool doLeaf = leafLvl > 0 && cam && light && glslSet && intT;
  const bool addUPtr = doLeaf && !uPtrF;
  if (!hash && !doLeaf) return false;

  uint32_t next = bound;
  auto nid = [&]() { return next++; };
  const uint32_t refId = nid(), newMain = nid(), lbl = nid(), callId = nid(), loadId = nid(), aId = nid(), dId = nid(), wId = nid(), weId = nid(), qId = nid(), sId = nid(),
                 fcId = nid(), xId = nid(), yId = nid(), txId = nid(), tyId = nid(), tId = nid(), f1Id = nid(), mId = nid(), hId = nid(), condId = nid(), killLbl = nid(),
                 mergeLbl = nid(), cHalf = nid(), cEps = nid(), cOne = nid(), cK1 = nid(), cK2 = nid(), cK3 = nid(), nvId = nid();
  const bool addBool = !boolT && !a2c;
  if (addBool) boolT = nid();
  const bool addPtr = addFc && !inPtr;
  if (addPtr) inPtr = nid();
  if (addFc) fcVar = nid();
  uint32_t cI0 = 0, cI1 = 0, cI2 = 0, cI37 = 0, cF025 = 0, cF133 = 0, cF0 = 0, cGain = 0, cTg = 0, cTb = 0;
  if (doLeaf) {
    cI0 = nid(); cI1 = nid(); cI2 = nid(); cI37 = nid(); cF025 = nid(); cF133 = nid(); cF0 = nid(); cGain = nid(); cTg = nid(); cTb = nid();
    if (addUPtr) uPtrF = nid();
  }

  out.assign(in.begin(), in.begin() + 5);
  out[3] = 0;
  size_t k = 5;
  while (k < in.size() && k != annotEnd) {
    const uint32_t wc = in[k] >> 16, op = in[k] & 0xFFFF;
    const size_t base = out.size();
    out.insert(out.end(), in.begin() + k, in.begin() + k + wc);
    if (op == kOpEntryPoint && in[k + 2] == entry) {
      out[base + 2] = newMain;
      if (addFc) {   // the new Input variable joins the interface (after the existing ids)
        out.push_back(fcVar);
        out[base] = ((uint32_t)(wc + 1) << 16) | kOpEntryPoint;
      }
    }
    if (op == kOpExecutionMode && in[k + 1] == entry) out[base + 1] = newMain;
    k += wc;
  }
  Emit(out, kOpDecorate, {refId, 1, 0});   // SpecId 0 (the game's alpha reference, as in PatchAlphaTest)
  if (addFc) Emit(out, kOpDecorate, {fcVar, 11, 15});
  while (k < firstFn) { const uint32_t wc = in[k] >> 16; out.insert(out.end(), in.begin() + k, in.begin() + k + wc); k += wc; }
  if (addBool) Emit(out, kOpTypeBool, {boolT});
  if (addPtr) Emit(out, kOpTypePointer, {inPtr, 1, vecT});
  if (addFc) Emit(out, kOpVariable, {inPtr, fcVar, 1});
  if (addUPtr) Emit(out, kOpTypePointer, {uPtrF, 2, floatT});
  Emit(out, kOpSpecConstant, {floatT, refId, 0});
  Emit(out, kOpConstant, {floatT, cHalf, FBits(0.5f)});
  Emit(out, kOpConstant, {floatT, cEps, FBits(1.0f / 512.0f)});
  Emit(out, kOpConstant, {floatT, cOne, FBits(1.0f)});
  Emit(out, kOpConstant, {floatT, cK1, FBits(0.06711056f)});
  Emit(out, kOpConstant, {floatT, cK2, FBits(0.00583715f)});
  Emit(out, kOpConstant, {floatT, cK3, FBits(52.9829189f)});
  if (doLeaf) {
    Emit(out, kOpConstant, {intT, cI0, 0});
    Emit(out, kOpConstant, {intT, cI1, 1});
    Emit(out, kOpConstant, {intT, cI2, 2});
    Emit(out, kOpConstant, {intT, cI37, 37});
    Emit(out, kOpConstant, {floatT, cF025, FBits(leafLvl > 8 ? -1.0f : -0.35f)});
    Emit(out, kOpConstant, {floatT, cF133, FBits(leafLvl > 8 ? 0.5f : 1.0f / 1.35f)});
    Emit(out, kOpConstant, {floatT, cF0, FBits(0.0f)});
    Emit(out, kOpConstant, {floatT, cGain, FBits(leafLvl > 8 ? 20.0f : 2.5f * (float)leafLvl / 8.0f)});
    Emit(out, kOpConstant, {floatT, cTg, FBits(leafLvl > 8 ? 0.0f : 0.9f)});
    Emit(out, kOpConstant, {floatT, cTb, FBits(leafLvl > 8 ? 0.0f : 0.35f)});
  }
  out.insert(out.end(), in.begin() + k, in.end());
  // wrapper
  Emit(out, kOpFunction, {voidT, newMain, 0, fnT});
  Emit(out, kOpLabel, {lbl});
  Emit(out, kOpFunctionCall, {voidT, callId, entry});
  Emit(out, kOpLoad, {vecT, loadId, var0});
  Emit(out, kOpCompositeExtract, {floatT, aId, loadId, 3});
  uint32_t colId = loadId;   // the colour vector the final store writes (leaf term applied below)
  auto emitLeaf = [&]() {
    uint32_t pc[6], ld[6];
    for (int n = 0; n < 6; n++) { pc[n] = nid(); ld[n] = nid(); }
    const uint32_t idxs[6][3] = {{cI0, cI37, cI0}, {cI0, cI37, cI1}, {cI0, cI37, cI2}, {cI0, cI1, cI0}, {cI0, cI1, cI1}, {cI0, cI1, cI2}};
    for (int n = 0; n < 6; n++) {
      Emit(out, 65 /*OpAccessChain*/, {uPtrF, pc[n], n < 3 ? cam : light, idxs[n][0], idxs[n][1], idxs[n][2]});
      Emit(out, kOpLoad, {floatT, ld[n], pc[n]});
    }
    const uint32_t m1 = nid(), m2 = nid(), m3 = nid(), s1 = nid(), dd = nid(), t0 = nid(), t1 = nid(), tc = nid(), t2 = nid(), mm = nid();
    Emit(out, kOpFMul, {floatT, m1, ld[0], ld[3]});
    Emit(out, kOpFMul, {floatT, m2, ld[1], ld[4]});
    Emit(out, kOpFMul, {floatT, m3, ld[2], ld[5]});
    Emit(out, kOpFAdd, {floatT, s1, m1, m2});
    Emit(out, kOpFAdd, {floatT, dd, s1, m3});
    uint32_t dUse = dd;
    if (!leafFlip) { dUse = nid(); Emit(out, 127 /*OpFNegate*/, {floatT, dUse, dd}); }   // c5[1] is the direction the light travels; toLight = -c5[1]
    Emit(out, kOpFSub, {floatT, t0, dUse, cF025});
    Emit(out, kOpFMul, {floatT, t1, t0, cF133});
    Emit(out, 12 /*OpExtInst*/, {floatT, tc, glslSet, 43 /*FClamp*/, t1, cF0, cOne});
    Emit(out, kOpFMul, {floatT, t2, tc, tc});
    Emit(out, kOpFMul, {floatT, mm, t2, cGain});
    uint32_t cur = colId;
    const uint32_t tint[3] = {cOne, cTg, cTb};
    for (uint32_t c = 0; c < 3; c++) {
      const uint32_t ch = nid(), g = nid(), add = nid(), sum = nid(), nv = nid();
      Emit(out, kOpCompositeExtract, {floatT, ch, cur, c});
      Emit(out, kOpFMul, {floatT, g, mm, tint[c]});
      Emit(out, kOpFMul, {floatT, add, ch, g});
      Emit(out, kOpFAdd, {floatT, sum, ch, add});
      Emit(out, kOpCompositeInsert, {vecT, nv, sum, cur, c});
      cur = nv;
    }
    colId = cur;
  };
  if (hash) {
    if (func == 5 || func == 7) Emit(out, kOpFSub, {floatT, dId, aId, refId}); else Emit(out, kOpFSub, {floatT, dId, refId, aId});
    Emit(out, kOpFwidth, {floatT, wId, aId});
    const uint32_t wsId = nid();   // half width ramp: fewer dithered pixels, the edge still gets a graded step
    Emit(out, kOpFMul, {floatT, wsId, wId, cHalf});
    Emit(out, kOpFAdd, {floatT, weId, wsId, cEps});
    Emit(out, kOpFDiv, {floatT, qId, dId, weId});
    Emit(out, kOpFAdd, {floatT, sId, qId, cHalf});
    if (a2c) {
      if (doLeaf) emitLeaf();
      Emit(out, kOpCompositeInsert, {vecT, nvId, sId, colId, 3});
      Emit(out, kOpStore, {var0, nvId});
      Emit(out, kOpReturn, {});
    } else {
      Emit(out, kOpLoad, {vecT, fcId, fcVar});
      Emit(out, kOpCompositeExtract, {floatT, xId, fcId, 0});
      Emit(out, kOpCompositeExtract, {floatT, yId, fcId, 1});
      Emit(out, kOpFMul, {floatT, txId, xId, cK1});
      Emit(out, kOpFMul, {floatT, tyId, yId, cK2});
      Emit(out, kOpFAdd, {floatT, tId, txId, tyId});
      Emit(out, kOpFRem, {floatT, f1Id, tId, cOne});
      Emit(out, kOpFMul, {floatT, mId, f1Id, cK3});
      Emit(out, kOpFRem, {floatT, hId, mId, cOne});
      Emit(out, kOpFOrdGreaterThan, {boolT, condId, sId, hId});
      Emit(out, kOpSelectionMerge, {mergeLbl, 0});
      Emit(out, kOpBranchConditional, {condId, mergeLbl, killLbl});
      Emit(out, kOpLabel, {killLbl});
      Emit(out, kOpKill, {});
      Emit(out, kOpLabel, {mergeLbl});
      if (doLeaf) { emitLeaf(); Emit(out, kOpStore, {var0, colId}); }
      Emit(out, kOpReturn, {});
    }
  } else {   // leaf term only: the game's own hard test, then the term
    const uint32_t cmp = func == 5 ? (uint32_t)kOpFOrdGreaterThan : func == 7 ? 190u : func == 2 ? 184u : 188u;
    Emit(out, cmp, {boolT, condId, aId, refId});
    Emit(out, kOpSelectionMerge, {mergeLbl, 0});
    Emit(out, kOpBranchConditional, {condId, mergeLbl, killLbl});
    Emit(out, kOpLabel, {killLbl});
    Emit(out, kOpKill, {});
    Emit(out, kOpLabel, {mergeLbl});
    emitLeaf();
    Emit(out, kOpStore, {var0, colId});
    Emit(out, kOpReturn, {});
  }
  Emit(out, kOpFunctionEnd, {});
  out[3] = next;
  return true;
}

// Pipeline bits for one draw (PipeDesc.pad2: bit 0 = hashed alpha edge, bit 1 = leaf debug flip, bits 4..7 = leaf gain level 0..8, 9 = debug paint; 0 = the game's own hard alpha test). Only
// opaque alpha-tested draws into the RGBA16F scene colour with a depth test qualify (blended particles, shadow casters, UI, the photo/Evaluation
// targets and 8 bit targets differ), and no photo frame does. The bits are part of the pipeline-key bytes, so the persisted cache stays valid for 0.
uint32_t FoliagePipeBits(int alphaFunc, float alphaRef, int32_t colorVkFmt, int32_t depthVkFmt, bool depthTest, int nColor, bool blend0) {
  if (alphaFunc == 0 || nColor < 1 || depthVkFmt == 0 || !depthTest || blend0 || alphaRef < 0.15f) return 0;
  if (colorVkFmt != 97 /* VK_FORMAT_R16G16B16A16_SFLOAT */) return 0;
  if (alphaFunc != 2 && alphaFunc != 4 && alphaFunc != 5 && alphaFunc != 7) return 0;
  const bool aa = FoliageAaEnabled();
  const int dbg = FoliageLeafDebugFlags();
  const int leaf = (dbg & 32) ? 9 : (int)(FoliageLeafStrength() * 8.0f + 0.5f);
  if (!aa && leaf <= 0) return 0;
  static std::atomic<int> s_photoKept{0}, s_ran{0};
  if (FxIsPhotoFrame()) {
    const int n = ++s_photoKept;
    if (n == 1 || n % 500 == 0) fprintf(stderr, "[foliage] photo frame: %d qualifying alpha-tested draws kept the game's own pipeline so far, %d variant draws on normal frames\n", n, s_ran.load());
    return 0;
  }
  ++s_ran;
  FxNoteRan();   // an effect variant is drawing this frame (A/B capture gating counters)
  return (aa ? 1u : 0u) | ((uint32_t)std::min(leaf, 9) << 4) | ((dbg & 16) ? 2u : 0u);   // bit 1 = debug flip (bits 8..16 belong to the parallax rows)
}

namespace {
std::mutex g_lblMu;
std::map<const void*, char> g_lbl;
}
float FoliageParticleBoost(const void* tex, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t* rel, int n) {
  if (w != 960 || h != 540 || (fmt != 0x29 && fmt != 0x34)) return 1.0f;
  char lab = 0;
  for (int i = 0; i < n; i++) {
    if (rel[i] == 0x5be358 || rel[i] == 0x5be378) lab = 'A';
    else if (rel[i] == 0x5bfde8 || rel[i] == 0x5bfe08) lab = 'B';
    else if (rel[i] == 0x5c09ac) lab = 'C';
  }
  if (!lab) return 1.0f;
  { std::lock_guard<std::mutex> g(g_lblMu); g_lbl[tex] = lab; }
  static const int sel = [] { const char* e = getenv("SNAP_SOFT_PARTICLES_SEL"); return e && *e ? atoi(e) : 6; }();   // bit 0 = A, 1 = B, 2 = C: the particle pass draws into C + the depth of B (B's colour is the downsample's own output)
  if (!SoftParticlesEnabled() || !((lab == 'A' ? 1 : lab == 'B' ? 2 : 4) & sel)) return 1.0f;
  return 2.0f;
}
void FoliageTextureGone(const void* tex) { std::lock_guard<std::mutex> g(g_lblMu); g_lbl.erase(tex); }
char FoliageTexLabel(const void* tex) {
  if (!tex) return 0;
  std::lock_guard<std::mutex> g(g_lblMu);
  auto it = g_lbl.find(tex);
  return it == g_lbl.end() ? 0 : it->second;
}

void FoliageLogDraw(const FoliageDrawInfo& d) {
  static const char* path = getenv("SNAP_FOLIAGE_LOG");
  if (!path || !*path) return;
  static std::mutex mu;
  static FILE* f = fopen(path, "w");
  static std::set<std::string> seen;
  char buf[900]; int n = snprintf(buf, sizeof buf, "rtl=%c dsl=%c vs=%s fs=%s af=%d ref=%.3f nc=%d rt0=f%x %ux%u s%u ds=f%x blend=%d(%d,%d,%d,%d) dw=%d dt=%d tex:", d.rtLabel ? d.rtLabel : '-', d.dsLabel ? d.dsLabel : '-', d.vs, d.fs, d.alphaFunc, d.alphaRef, d.nColor, d.colorFmt0,
                                  d.rtW, d.rtH, d.rtSamples, d.depthFmt, (int)d.blend0, d.srcRgb, d.dstRgb, d.srcA, d.dstA, (int)d.depthWrite, (int)d.depthTest);
  for (int i = 0; i < d.nTex && n < 800; i++) n += snprintf(buf + n, sizeof buf - n, " b%d:f%x:%ux%u", d.tex[i].binding, d.tex[i].fmt, d.tex[i].w, d.tex[i].h);
  std::lock_guard<std::mutex> g(mu);
  if (f && seen.insert(buf).second) { fprintf(f, "[p%d n=%d i=%d] %s\n", d.present, d.nVerts, d.nInst, buf); fflush(f); }
}

}  // namespace nvn
