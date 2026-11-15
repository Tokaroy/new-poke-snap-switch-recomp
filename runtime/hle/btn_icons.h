// SPDX-License-Identifier: MIT
// Button icon mod, logic side (analysis/mods/button_icons.md): which glyph (Xbox or PlayStation) shows for each Switch button prompt.
// Pure host code with no guest types, so the tests can drive it. The renderer (runtime/nvn/nvn_btnicon.cpp) reads the result through
// snap_btnicon_glyph(); the art lives in nvn_btnicon_atlas.h in the same order as enum Glyph.
//
// The icon shows the physical button the player has to press under the CURRENT binding. The game asks for a Switch button (A, ZR ...);
// Mapping.buttons[action] lists the host sources bound to it, so the glyph comes from the first controller source in that list:
//   default binding, Switch A (east)  ->  Xbox B / PlayStation Circle        (position, not letter)
//   face_layout 1 (Nintendo by label) ->  Xbox A / PlayStation Cross
//   rebound action                    ->  whatever button it is bound to now
// An action with no controller source (keyboard or mouse only), or the "Switch" style, keeps the game's own icon.
#pragma once
#include <string>

#include "input_core.h"

namespace hle::icons {

enum Style : int { kAuto = 0, kSwitch, kXbox, kPlayStation, kStyleCount };   // the setting "button_icons"
enum Family : int { kFamNone = 0, kFamSwitch, kFamXbox, kFamPlayStation };   // what the connected pad is (RawInput::pad_family)

// Order = the columns of the atlas (scripts/mods/make_button_icons.py GLYPHS).
enum Glyph : int {
  kFaceBottom = 0, kFaceRight, kFaceLeft, kFaceTop, kL1, kR1, kL2, kR2, kMenu, kView,
  kDpadUp, kDpadDown, kDpadLeft, kDpadRight, kStickL, kStickR, kStickLPress, kStickRPress,
  kStickLUp, kStickLDown, kStickLLeft, kStickLRight, kStickRUp, kStickRDown, kStickRLeft, kStickRRight,
  kGlyphCount
};

Style parse_style(const std::string& s);           // "auto" "switch" "xbox" "playstation" (case-insensitive, "ps" too); anything else = Auto
const char* style_name(Style s);                   // the stored value
// The style that is actually drawn: Auto follows the connected pad (Switch pad, no pad or an unknown one = Switch); never returns Auto.
Style resolve_style(Style chosen, Family detected, bool pad_connected);

// Glyph for a controller source (position-named PadButton), kGlyphCount when the button has no glyph (Guide).
int glyph_of_pad_button(int pad_button);
// Glyph for a bindable action (Button enum or 16..23 stick direction) under mapping m: the first controller source, else kGlyphCount.
int glyph_of_action(const input::Mapping& m, int action);

// Snapshot read by the renderer thread. Setters are called from the input hub (mapping, pad) and the settings layer (style); each
// recomputes the table. Thread safe.
void set_mapping(const input::Mapping& m);
void set_pad(Family f, bool connected);
void set_style(Style s);
Style style();                                      // as chosen
Style active_style();                               // as resolved
// Packed result for a Switch button (Button enum value 0..15): (row << 8) | glyph with row 0 = Xbox, 1 = PlayStation, or -1 = keep the
// game's own icon.
int packed_glyph(int button);

}  // namespace hle::icons

extern "C" int snap_btnicon_glyph(int button);       // = icons::packed_glyph, for the renderer
extern "C" int snap_btnicon_active();                // 1 while Xbox or PlayStation pictures are drawn (a cheap per-draw check)
