// SPDX-License-Identifier: MIT
// Drawing helpers for the game-style look (palette in ui_theme.h, measurements in analysis/ui/style_ref.md): a soft card shadow and the
// controller-prompt glyphs (orange chevron with a white disc for face buttons, grey pill for shoulder buttons).
#include <cstring>

#include "hle/btn_icons.h"
#include "ui_i18n.h"
#include "ui_internal.h"

namespace snap::ui {

static ImU32 col32(const ImVec4& c) { return ImGui::GetColorU32(c); }

// ---- Button icons setting (analysis/mods/button_icons.md): the menus' own prompts follow the chosen icon family. Drawn with
// ImGui primitives (colored discs, symbols, grey pills) so no texture is needed; the game's prompts use the Kenney art instead.
namespace {
enum Fam { kFamSwitch = 0, kFamXbox, kFamPS };
int family() {
  switch (hle::icons::active_style()) {
    case hle::icons::kXbox: return kFamXbox;
    case hle::icons::kPlayStation: return kFamPS;
    default: return kFamSwitch;
  }
}
bool is_face(int b) { return b >= hle::input::kPadSouth && b <= hle::input::kPadNorth; }
// Text of the grey pill a non-face button is drawn as; nullptr when the button has no glyph (D-pad, stick directions, Guide).
const char* pill_text(int fam, int b) {
  const bool x = fam == kFamXbox;
  switch (b) {
    case hle::input::kPadLShoulder: return x ? "LB" : "L1";
    case hle::input::kPadRShoulder: return x ? "RB" : "R1";
    case hle::input::kPadLTrigger: return x ? "LT" : "L2";
    case hle::input::kPadRTrigger: return x ? "RT" : "R2";
    case hle::input::kPadBack: return x ? "View" : "Create";
    case hle::input::kPadStart: return x ? "Menu" : "Options";
    case hle::input::kPadLStick: return x ? "LS" : "L3";
    case hle::input::kPadRStick: return x ? "RS" : "R3";
    default: return nullptr;
  }
}
}  // namespace

bool style_pad_glyphs() { return family() != kFamSwitch; }

float style_pad_glyph_width(int b, float h) {
  const int fam = family();
  if (fam == kFamSwitch) return 0.0f;
  if (is_face(b)) return h;
  const char* t = pill_text(fam, b);
  return t ? ImGui::CalcTextSize(t).x + h * 0.7f : 0.0f;
}

float style_pad_glyph(ImDrawList* dl, ImVec2 p, float h, int b) {
  const int fam = family();
  const float w = style_pad_glyph_width(b, h);
  if (w <= 0.0f) return 0.0f;
  const float r = h * 0.5f;
  const ImVec2 c(p.x + r, p.y + r);
  if (is_face(b)) {
    const int i = b - hle::input::kPadSouth;   // south, east, west, north
    if (fam == kFamXbox) {
      static const char* const letter[4] = {"A", "B", "X", "Y"};
      static const ImU32 color[4] = {IM_COL32(104, 176, 36, 255), IM_COL32(214, 62, 46, 255), IM_COL32(32, 138, 224, 255), IM_COL32(236, 170, 20, 255)};
      dl->AddCircleFilled(c, r, color[i], 24);
      const ImVec2 ts = ImGui::CalcTextSize(letter[i]);
      dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(255, 255, 255, 255), letter[i]);
    } else {
      static const ImU32 color[4] = {IM_COL32(120, 140, 240, 255), IM_COL32(236, 96, 96, 255), IM_COL32(236, 112, 208, 255), IM_COL32(70, 206, 156, 255)};
      const float th = r * 0.26f;
      const ImU32 k = color[i];
      dl->AddCircleFilled(c, r, IM_COL32(58, 58, 68, 255), 24);
      if (i == 0) {                                                   // cross
        const float a = r * 0.4f;
        dl->AddLine(ImVec2(c.x - a, c.y - a), ImVec2(c.x + a, c.y + a), k, th);
        dl->AddLine(ImVec2(c.x - a, c.y + a), ImVec2(c.x + a, c.y - a), k, th);
      } else if (i == 1) {                                            // circle
        dl->AddCircle(c, r * 0.46f, k, 20, th);
      } else if (i == 2) {                                            // square
        const float a = r * 0.4f;
        dl->AddRect(ImVec2(c.x - a, c.y - a), ImVec2(c.x + a, c.y + a), k, 0.0f, 0, th);
      } else {                                                        // triangle
        dl->AddTriangle(ImVec2(c.x, c.y - r * 0.42f), ImVec2(c.x - r * 0.44f, c.y + r * 0.3f), ImVec2(c.x + r * 0.44f, c.y + r * 0.3f), k, th);
      }
    }
    return w;
  }
  const char* t = pill_text(fam, b);
  const ImVec2 ts = ImGui::CalcTextSize(t);
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), fam == kFamXbox ? IM_COL32(64, 66, 74, 255) : IM_COL32(58, 58, 68, 255), r);
  dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), IM_COL32(255, 255, 255, 255), t);
  return w;
}

void style_shadow(ImDrawList* dl, ImVec2 pos, ImVec2 size, float round) {
  const float s = ImGui::GetFontSize() / theme().font_px;
  for (int i = 0; i < 7; ++i) {   // stacked translucent rounded rects: cheap soft shadow, offset a little downward like the game's cards
    const float g = (3.0f + i * 3.5f) * s;
    dl->AddRectFilled(ImVec2(pos.x - g, pos.y - g + 8.0f * s), ImVec2(pos.x + size.x + g, pos.y + size.y + g + 8.0f * s), IM_COL32(60, 45, 30, 9), round + g);
  }
}

float style_prompt(const char* glyph, const char* label) {
  const Theme& t = theme();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  const float h = fs * 1.35f;
  const bool face = std::strlen(glyph) == 1;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float y0 = p.y + (ImGui::GetTextLineHeight() - h) * 0.5f;
  float w;
  // Xbox or PlayStation chosen: the menu buttons are physical (ImGui navigation: south selects, east goes back, bumpers switch page)
  const int pb = !std::strcmp(glyph, "A") ? hle::input::kPadSouth : !std::strcmp(glyph, "B") ? hle::input::kPadEast
               : !std::strcmp(glyph, "LB") ? hle::input::kPadLShoulder : !std::strcmp(glyph, "RB") ? hle::input::kPadRShoulder : -1;
  if (pb >= 0 && style_pad_glyphs()) {
    w = style_pad_glyph(dl, ImVec2(p.x, p.y + (ImGui::GetTextLineHeight() - h) * 0.5f), h, pb);
  } else if (face) {
    w = h * 1.45f;
    const float nose = h * 0.42f;   // chevron pointing right: the game's A and B prompt shape
    ImVec2 pts[5] = {{p.x, y0}, {p.x + w - nose, y0}, {p.x + w, y0 + h * 0.5f}, {p.x + w - nose, y0 + h}, {p.x, y0 + h}};
    dl->AddConvexPolyFilled(pts, 5, col32(t.accent_hover));
    dl->AddPolyline(pts, 5, IM_COL32(150, 100, 70, 255), ImDrawFlags_Closed, 1.5f);
    const ImVec2 c(p.x + (w - nose) * 0.5f + h * 0.12f, y0 + h * 0.5f);
    dl->AddCircleFilled(c, h * 0.34f, IM_COL32(255, 250, 245, 255), 20);
    const ImVec2 ts = ImGui::CalcTextSize(glyph);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col32(t.title), glyph);
  } else {
    const ImVec2 ts = ImGui::CalcTextSize(glyph);
    w = ts.x + h * 0.7f;
    dl->AddRectFilled(ImVec2(p.x, y0), ImVec2(p.x + w, y0 + h), col32(t.text_dim), h * 0.5f);
    dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, y0 + (h - ts.y) * 0.5f), IM_COL32(255, 255, 255, 255), glyph);
  }
  ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
  ImGui::SameLine(0, fs * 0.35f);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(t.text_dim, "%s", (label && *label) ? tr(label) : label);
  ImGui::SameLine(0, fs * 1.2f);
  return w;
}

void style_footer_prompts(bool tabs, bool back) {
  style_prompt("A", "Select");
  if (back) style_prompt("B", "Back");
  if (tabs) { style_prompt("LB", ""); style_prompt("RB", "Switch page"); }
  ImGui::NewLine();
}

}  // namespace snap::ui
