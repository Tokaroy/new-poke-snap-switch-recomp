// SPDX-License-Identifier: MIT
// The F1 menu's game skin (analysis/ui/f1_skin.md): the look of the game's own Settings screen (dark slate panel, rounded tab strip with
// L / R tags, grey bars with pointed ends that turn green under the cursor, yellow triangle-pattern selection, white drop-down pills, a bar
// of description text at the bottom). Everything here only draws; the rows, values and settings logic stay in ui_options.cpp and friends.
#pragma once
#include <string>

#include "imgui.h"

namespace snap::ui {

struct SkinPal {
  ImVec4 backdrop, panel, panel_edge, tab_idle, tab_idle_text, tab_sel_text, ornament;
  ImVec4 row, row_text, row_focus, row_focus_text, row_dim_text;
  ImVec4 yellow, yellow_tri, yellow_text, off_pill, off_text, white_pill, white_text, arrow;
  ImVec4 bar, bar_text, bar_dim, tag, tag_edge, tag_text, tag_inner;
  ImVec4 good, warn, error;
};
const SkinPal& skin_pal();     // the dark game palette, or the black and white one while the high contrast row is on

ImU32 skin_col(const ImVec4& c, float mul_alpha = 1.0f);   // sRGB-aware (the swapchain may be sRGB), times the style alpha
float skin_ease_out(float t);                              // 0..1 -> 0..1, cubic ease out
float skin_approach(float cur, float target, float rate);  // exponential approach, frame-rate independent

// Style and theme for the pages drawn inside the menu window: the ImGui widgets (checkboxes, buttons, tables) and the theme colors the
// pages use for headings and status text become legible on the dark panel. Restores both on destruction.
struct SkinScope {
  SkinScope();
  ~SkinScope();
  SkinScope(const SkinScope&) = delete;
  SkinScope& operator=(const SkinScope&) = delete;
 private:
  struct Saved;
  Saved* s_;
};

// Tab strip. Draws at the cursor over `width`; returns the tab index clicked (L / R tags step by one), -1 for none. `focus_pad` highlights the
// L and R tags as the controller prompts.
int skin_tabs(const char* const* names, int n, int cur, float width);

// One row of a page: a grey bar with pointed ends behind the content, green (eased) while focused. Content is submitted between begin and
// end exactly like ordinary ImGui widgets; the bar is drawn behind it.
struct SkinRow {
  ImDrawList* dl = nullptr;
  ImVec2 p;
  float w = 0, h = 0;
  float e = 0;        // focus, eased 0..1
  ImGuiID id = 0;
};
float skin_row_height();                                    // pixels
float skin_control_height();                                // height of the pills and sliders inside a row
void skin_row_begin(SkinRow& r, ImGuiID id, bool focused);     // the cursor is left where the row starts; draw the label and controls at your own positions
ImU32 skin_row_text(const SkinRow& r, float mul_alpha = 1.0f);   // label color: white, turning dark as the green focus eases in
void skin_row_end(SkinRow& r, bool disabled);

// Row controls. Each is one focusable item; the caller decides what a click or a step means.
// Segmented control (On / Off, Windowed / Borderless): `sel` is the selected label; returns the label clicked with the mouse, -1 for none.
int skin_segmented(const char* id, const char* const* labels, int n, int sel, bool disabled);
// White pill with a small arrow (a drop-down); true when it was activated (click, Enter or the pad's A), then open your popup.
bool skin_dropdown(const char* id, const char* text, float width, bool disabled);
// Bar filled in yellow up to frac; `text` is centered. Dragging with the mouse changes frac_io; returns true while it changes, `released`
// is set on the frame the mouse lets go.
bool skin_slider(const char* id, float* frac_io, const char* text, float width, bool disabled, bool* released);

// Description line of the focused row (the game shows it in the bar at the bottom of its Settings screen). Set from any row each frame,
// drawn once by skin_help_draw; cleared when drawn.
void skin_help_set(const std::string& text, const std::string& extra, bool extra_warn);
bool skin_help_pending();
void skin_help_draw(ImVec2 pos, float w, float h);

// Orange L / R tag (or the Xbox / PlayStation button picture) and the B Back / A Select prompts.
void skin_shoulder_tag(ImDrawList* dl, ImVec2 pos, float h, bool right, bool pressed);
// Draws the Back tag and the small status line (right aligned) inside the description bar; returns true when the B tag was clicked.
bool skin_bar_prompts(ImVec2 r_min, ImVec2 r_max, bool back, const char* status, bool status_good);

// Open animation: call once when the menu opens, read the factor every frame (0 -> 1 over ~0.16 s).
void skin_open_started();
// Pad / keyboard page switch: the cursor lands on the new page's first row (request after L / R; apply right after the menu window's Begin).
void skin_cursor_first_row();
void skin_cursor_apply();
float skin_open_factor();

}  // namespace snap::ui
