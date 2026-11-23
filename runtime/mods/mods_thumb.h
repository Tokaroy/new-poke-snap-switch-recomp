// SPDX-License-Identifier: MIT
// Thumbnail source for a pack's texture (project code, ours): reads one .nutexb, undoes the Tegra block-linear layout of
// one small mip and hands back the blocks in a form Direct3D 11 can create a texture from (BC1-BC5, BC7, RGBA8). The
// launcher's Mods page uses it to show two packs' versions of one texture side by side. Local only: nothing is written
// or sent. Host-only, no dependency on the lifted game; tests/mods/mods_tests.cpp covers it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mods {

struct Thumb {
  uint32_t dxgi = 0;               // DXGI_FORMAT of `blocks` (28 RGBA8, 71 BC1, 74 BC2, 77 BC3, 80 BC4, 83 BC5, 98 BC7; always the UNORM form)
  int w = 0, h = 0;                // size of the chosen mip in texels, rounded up to whole blocks
  int src_w = 0, src_h = 0;        // the texture's real top size
  int mips = 0;
  int mip = 0;                     // index of the chosen mip
  uint32_t pitch = 0;              // bytes per row of blocks
  std::string format;              // "BC7", "BC1", "RGBA8" ... for the label
  std::vector<uint8_t> blocks;     // pitch * (h / block height) bytes, linear
};

// `want` is the longest side wanted: the smallest mip at least that long is chosen (the last one when none is).
// False with *err set for a file that is not a plain 2D nutexb or uses a format the preview does not read (BC6, float).
bool nutexb_thumb(const uint8_t* data, size_t n, int want, Thumb* out, std::string* err);
bool nutexb_thumb_file(const std::string& path, int want, Thumb* out, std::string* err);

}  // namespace mods
