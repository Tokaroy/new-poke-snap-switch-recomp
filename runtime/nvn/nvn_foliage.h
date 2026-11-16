// SPDX-License-Identifier: MIT
// Foliage and particle display upgrades (project code, ours): hashed alpha / alpha-to-coverage for alpha-tested draws, leaf
// translucency, full-resolution soft particles. Every effect is display only: photo frames (FxIsPhotoFrame), the shadow-map passes and
// the Evaluation targets keep the game's own draw. Switches (all off by default; read through the settings exports, see settings.cpp):
//   SNAP_FOLIAGE_AA=1             alpha-tested draws in the scene pass dither/cover their edge instead of a hard discard
//   SNAP_LEAF_TRANSLUCENCY=0..1   strength of the back-lit leaf term (0 = off)
//   SNAP_SOFT_PARTICLES=1         particles fade out where they meet geometry and are drawn at full render resolution
//   SNAP_FOLIAGE_LOG=<file>       debug: one line per distinct alpha-tested / blended scene draw combination
#pragma once
#include <cstdint>
#include <vector>

namespace nvn {

struct FoliageTex { int binding; uint32_t fmt, w, h; };
struct FoliageDrawInfo {
  int present;
  const char* vs; const char* fs;
  int alphaFunc; float alphaRef;
  int nColor; uint32_t colorFmt0, rtW, rtH, rtSamples; uint32_t depthFmt;
  bool blend0, depthWrite, depthTest;
  int srcRgb, dstRgb, srcA, dstA;
  int nTex; FoliageTex tex[8];
  int nVerts, nInst;
  char rtLabel, dsLabel;   // FoliageTexLabel of colour 0 / depth ('A', 'B' = the two half-resolution particle target pairs, 0 = other)
};
void FoliageLogDraw(const FoliageDrawInfo& d);   // debug census (SNAP_FOLIAGE_LOG), cheap when unset

// SPIR-V wrapper for alpha-tested fragment modules (nvn_foliage.cpp): `in` is the translated module, `out` replaces it; spec constant 0 = alpha reference.
// hash: dithered / covered edge; leaf: 0..8 back-lit gain level (needs the scene light and camera constant buffers; ignored when the module has none).
bool PatchAlphaHash(const std::vector<uint32_t>& in, int func, bool a2c, bool hash, int leaf, std::vector<uint32_t>& out);
// PipeDesc.pad2 value for one draw (0 = the game's own hard alpha test; bit 0 = hashed edge, bits 4..7 = leaf gain level); formats are Vulkan format values.
uint32_t FoliagePipeBits(int alphaFunc, float alphaRef, int32_t colorVkFmt, int32_t depthVkFmt, bool depthTest, int nColor, bool blend0);

// Full-resolution particles (SNAP_SOFT_PARTICLES): the game draws particles into 960x540 targets (RGBA16F colour + 32 bit float depth, a downsample
// of the scene depth) and composites them back; those targets are made `boost` times larger so the particle layer has the scene's resolution.
// Called from TextureInitialize with the guest call chain (relative to main); returns the extra scale (1 = unchanged) and labels the texture.
float FoliageParticleBoost(const void* tex, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t* chainRel, int n);
void FoliageTextureGone(const void* tex);
char FoliageTexLabel(const void* tex);

bool FoliageAaEnabled();    // SNAP_FOLIAGE_AA
float FoliageLeafStrength();   // SNAP_LEAF_TRANSLUCENCY
int FoliageLeafDebugFlags();   // SNAP_LEAF_FLIP (16) | SNAP_LEAF_DEBUG (32): debug switches for the leaf term
bool SoftParticlesEnabled();   // SNAP_SOFT_PARTICLES

}  // namespace nvn
