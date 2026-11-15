// SPDX-License-Identifier: MIT
// Button icon mod, renderer side (analysis/mods/button_icons.md): a UI draw that samples one of the game's Switch button glyph
// textures samples a generated twin (Xbox or PlayStation glyph) instead. Cosmetic only; guest memory, saves and input are untouched.
#pragma once
#include <cstdint>
struct BtnDraw { const float* vbf = nullptr; int rtW = 0, rtH = 0, prog = -1; };   // vbf: host view of vertex stream 0 (UI sprites: 14 floats per vertex, 1920x1080 pixels)
namespace nvn {
struct TextureImpl;
bool BtnIconActive();                                              // trace on, or the setting is not "switch"
TextureImpl* BtnIconSubstitute(TextureImpl* t, const BtnDraw& d);  // t itself when nothing applies
void BtnIconForget(TextureImpl* t);                                // DestroyTexture: free the twins of t
}
