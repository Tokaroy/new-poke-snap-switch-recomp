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
// Keyboard style (Automatic while no controller is connected, or while the last input came from the keyboard or mouse): the prompt shows a key cap with
// the first keyboard key bound to the action (else its first mouse button), e.g. L, Enter, Shift, LMB. The label comes through snap_btnicon_label().
#pragma once
#include <string>

#include "input_core.h"

namespace hle::icons {

enum Style : int { kAuto = 0, kSwitch, kXbox, kPlayStation, kKeyboard, kStyleCount };   // the setting "button_icons"; kKeyboard draws key caps (keyboard and mouse players)
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
// Automatic: no pad connected = Keyboard; a pad that was not the last device used (kbm_last) = Keyboard too.
Style resolve_style(Style chosen, Family detected, bool pad_connected, bool kbm_last = false);

// Glyph for a controller source (position-named PadButton), kGlyphCount when the button has no glyph (Guide).
int glyph_of_pad_button(int pad_button);
// Glyph for a bindable action (Button enum or 16..23 stick direction) under mapping m: the first controller source, else kGlyphCount.
int glyph_of_action(const input::Mapping& m, int action);

// Snapshot read by the renderer thread. Setters are called from the input hub (mapping, pad) and the settings layer (style); each
// recomputes the table. Thread safe.
void set_mapping(const input::Mapping& m);
void set_pad(Family f, bool connected);
std::string label_text(int id);                   // key cap text of a keyboard glyph id (packed row 2), "" when unknown
void set_last_input(bool keyboard_or_mouse);       // the Hub: the newest input came from the keyboard or mouse (true) or a pad (false)
// Key cap text for a bindable action under mapping m ("L", "Enter", "Shift", "LMB"), empty when it has no keyboard or mouse source.
std::string label_of_action(const input::Mapping& m, int action);
void set_style(Style s);
Style style();                                      // as chosen
Style active_style();                               // as resolved
// Packed result for a Switch button (Button enum value 0..15): (row << 8) | glyph with row 0 = Xbox, 1 = PlayStation, or -1 = keep the
// game's own icon.
int packed_glyph(int button);

// Message text (the game's font draws its Switch button letters as private-use codepoints U+E0E0 A, U+E0E1 B, U+E0E2 X, U+E0E3 Y): the codepoint whose letter
// the controller really shows. Xbox style (chosen, or Automatic with an Xbox pad connected) under the CURRENT binding: the game's "X" (Switch X, bound to the
// pad's top button by default) reads "Y". Every other code, and every other style, comes back unchanged (the font has no PlayStation shapes, key caps or LB / RB).
unsigned text_glyph_code(unsigned code);

}  // namespace hle::icons

extern "C" int snap_btnicon_glyph(int button);
extern "C" const char* snap_btnicon_label(int id);   // key cap text of a keyboard glyph (packed row 2, low byte = id), never null       // = icons::packed_glyph, for the renderer
extern "C" int snap_btnicon_active();                // 1 while Xbox or PlayStation pictures are drawn (a cheap per-draw check)
