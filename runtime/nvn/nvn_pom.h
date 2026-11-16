// SPDX-License-Identifier: MIT
// Parallax occlusion mapping for ground, rock and bark (nvn_pom.cpp, docs/pom.md). Off unless SNAP_POM=low|high (settings key gfx_pom).
#pragma once
#include <cstdint>
#include <vector>

namespace nvn {
struct TextureImpl;
struct ReplayCtx;

// 0 off, 1 low, 2 high (SNAP_POM=off|low|high or 0|1|2; F1 > Graphics "Parallax detail", launcher "Parallax detail").
int PomLevel();

// Called for every sampled texture before its upload (ResolveHandle): looks the texture's content key up in pom_table.txt (ground, rock, bark).
void PomNoteTexture(TextureImpl* t);

// 128 bytes of fragment push constants at offset 128 (camera, sun and fade, see nvn_pom.cpp). Filled once per scene frame.
struct PomPush { float m[8][4]; };

// Pipeline bits for the draw being built (OR into PipeDesc.pad2, bits 8..16): level in bits 8..9, the baseColor sampler's handle index in bits 10..16.
// 0 = no parallax for this draw (off, not a scene draw, no class texture, camera unknown). `texClass`: the class of the first bound table texture.
uint32_t PomPipeBits(TextureImpl* rt0, int level, int handleIndex, uint8_t texClass);
// Patches the fragment program: the material UV (the coordinate of the sampler with this handle index) becomes the parallax-shifted one, the base color is
// darkened by the self-shadow. False (out untouched) when the program does not have the expected shape.
bool PatchPom(const std::vector<uint32_t>& in, int handleIndex, int level, std::vector<uint32_t>& out);
// Fills the push constants for this frame; false while the scene camera is unknown.
bool PomFillPush(PomPush* out);
// Pipeline layouts carry a fragment push range of this many bytes (RT shadows use the first 128, parallax the second 128); 0 = the device cannot (limit < 256).
uint32_t PomPushRangeBytes(uint32_t deviceLimit);
}  // namespace nvn
