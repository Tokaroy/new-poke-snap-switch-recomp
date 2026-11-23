// SPDX-License-Identifier: MIT
#include "mods_thumb.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "../nvn/nvn_tex.h"

namespace mods {
namespace {
constexpr size_t kFooter = 0xb0;
constexpr uint32_t kMagic = 0x54455820;   // " XET"

struct Fmt { uint32_t id; uint32_t bpb, bw, bh; uint32_t dxgi; const char* name; };
// nutexb format id -> block size and the UNORM DXGI format Direct3D 11 reads it as (table of the builder: builder/snapbuild/hdtex_nutexb.py).
constexpr Fmt kFmts[] = {
    {0x400, 4, 1, 1, 28, "RGBA8"}, {0x405, 4, 1, 1, 28, "RGBA8"},
    {0x480, 8, 4, 4, 71, "BC1"},   {0x485, 8, 4, 4, 71, "BC1"},
    {0x490, 16, 4, 4, 74, "BC2"},  {0x495, 16, 4, 4, 74, "BC2"},
    {0x4a0, 16, 4, 4, 77, "BC3"},  {0x4a5, 16, 4, 4, 77, "BC3"},
    {0x180, 8, 4, 4, 80, "BC4"},   {0x280, 16, 4, 4, 83, "BC5"},
    {0x4e0, 16, 4, 4, 98, "BC7"},  {0x4e5, 16, 4, 4, 98, "BC7"},
};

uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
}  // namespace

bool nutexb_thumb(const uint8_t* data, size_t n, int want, Thumb* out, std::string* err) {
  auto fail = [&](const char* m) { if (err) *err = m; return false; };
  if (!data || n < kFooter) return fail("too short for a nutexb");
  const uint8_t* f = data + n - kFooter;
  // footer: +0x84 width, height, depth, format, dim, mips, tile, layers, data_size, then " XET"
  const uint32_t w = rd32(f + 0x84), h = rd32(f + 0x88), d = rd32(f + 0x8c), fmt = rd32(f + 0x90), dim = rd32(f + 0x94);
  const uint32_t mips = rd32(f + 0x98), tile = rd32(f + 0x9c), layers = rd32(f + 0xa0), dsz = rd32(f + 0xa4);
  if (rd32(f + 0xa8) != kMagic) return fail("not a nutexb (footer magic)");
  if (dim != 2 || d != 1 || layers != 1) return fail("only plain 2D textures are previewed");
  if (tile == 0) return fail("linear layout is not previewed");
  if (w == 0 || h == 0 || w > 16384 || h > 16384 || mips == 0 || mips > 16) return fail("odd texture size");
  if (dsz > n - kFooter) return fail("data size past the end of the file");
  const Fmt* ft = nullptr;
  for (const Fmt& x : kFmts) if (x.id == fmt) ft = &x;
  if (!ft) return fail("format not previewed");
  const nvn::SurfaceLayout lay = nvn::ComputeSurfaceLayout(w, h, 1, 1, mips, ft->bpb, ft->bw, ft->bh);
  if (lay.total > n - kFooter) return fail("layout is larger than the data");
  // the smallest mip that is at least `want` long, and at least one whole block
  size_t pick = 0;
  for (size_t m = 0; m < lay.levels.size(); m++) {
    const nvn::LevelLayout& l = lay.levels[m];
    if (ft->bw > 1 && (l.w < ft->bw || l.h < ft->bh) && m > 0) break;
    pick = m;
    if ((int)std::max(l.w, l.h) <= std::max(want, 1)) break;
  }
  const nvn::LevelLayout& l = lay.levels[pick];
  Thumb t;
  t.dxgi = ft->dxgi;
  t.w = (int)(l.wb * ft->bw);
  t.h = (int)(l.hb * ft->bh);
  t.src_w = (int)w; t.src_h = (int)h; t.mips = (int)mips; t.mip = (int)pick;
  t.pitch = l.wb * ft->bpb;
  t.format = ft->name;
  t.blocks.assign((size_t)t.pitch * l.hb, 0);
  nvn::DeswizzleLevel(data + l.offset, t.blocks.data(), l, ft->bpb);
  *out = std::move(t);
  return true;
}

bool nutexb_thumb_file(const std::string& path, int want, Thumb* out, std::string* err) {
  std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) { if (err) *err = "cannot open the file"; return false; }
  f.seekg(0, std::ios::end);
  const std::streamoff sz = f.tellg();
  if (sz <= 0 || sz > (std::streamoff)512 * 1024 * 1024) { if (err) *err = "unreadable size"; return false; }
  f.seekg(0);
  std::vector<uint8_t> b((size_t)sz);
  if (!f.read((char*)b.data(), sz)) { if (err) *err = "cannot read the file"; return false; }
  return nutexb_thumb(b.data(), b.size(), want, out, err);
}

}  // namespace mods
