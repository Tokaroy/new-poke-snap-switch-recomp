// SPDX-License-Identifier: MIT
// "Save state" tab of the F1 overlay and the on-screen note for save states (docs/save-states.md, stage 3). One slot, kept in memory for this
// run only: F5 (or the tab's Save button) stores the game as it is, F8 (or Load) puts it back, so one photo can be tried again. Nothing is
// written to disk and the slot is gone when the game closes. The work itself is runtime/os/os_state.cpp; this file only asks for it and
// reports the result.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

#include "ui_internal.h"
#include "ui_theme.h"

extern "C" {
struct SnapStateInfo {   // same layout as runtime/os/os_state.cpp
  int busy, has_state, last_op, last_ok, saves, loads, done;
  unsigned long long last_done_ms, saved_ms;
  char last_msg[112];
};
void snap_state_request(int op, const char* name);
void snap_state_info(SnapStateInfo* out);
}

namespace snap::ui {

namespace {

constexpr const char* kSlot = "player";
constexpr double kNoteSecs = 6.0;

double now_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Note {
  int seen_done = 0;
  double until = 0;
  bool bad = false;
  std::string text;
} N;

// What the player reads for a refusal: the reason from the state code, in a sentence.
std::string refusal_text(int op, const char* why) {
  std::string s = op == 1 ? "Could not save the state: " : "Could not load the state: ";
  s += (why && *why) ? why : "unknown reason";
  s += ". The game goes on as it was.";
  return s;
}

}  // namespace

// Once per overlay frame. True while the note is on screen.
bool state_note_on() {
  SnapStateInfo si{};
  snap_state_info(&si);
  const double now = now_s();
  if (si.busy) {
    N.text = "Working on the save state...";
    N.bad = false;
    N.until = now + kNoteSecs;
    return true;
  }
  if (si.done != N.seen_done) {
    N.seen_done = si.done;
    N.bad = !si.last_ok;
    if (si.last_ok) {
      N.text = si.last_op == 1 ? "State saved. F8 loads it again. One slot, kept until you close the game."
                               : "State loaded. F5 saves a new one.";
    } else {
      N.text = refusal_text(si.last_op, si.last_msg);
    }
    N.until = now + kNoteSecs;
  }
  return now < N.until;
}

void draw_state_note(ImVec2 size) {
  const float k = std::max(0.75f, size.y / 1080.0f);
  ImFont* font = ImGui::GetFont();
  const float fs = 17.0f * k, pad = 8.0f * k;
  const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, N.text.c_str());
  const float x = (size.x - ts.x) * 0.5f, y = size.y - ts.y - pad * 2 - 40.0f * k;
  const float fade = (float)std::clamp((N.until - now_s()) / 1.0, 0.0, 1.0);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const int a = (int)(220 * fade);
  dl->AddRectFilled(ImVec2(x - pad * 1.5f, y), ImVec2(x + ts.x + pad * 1.5f, y + ts.y + pad * 2), IM_COL32(20, 20, 24, a), 6.0f * k);
  dl->AddRect(ImVec2(x - pad * 1.5f, y), ImVec2(x + ts.x + pad * 1.5f, y + ts.y + pad * 2),
              N.bad ? IM_COL32(230, 90, 70, (int)(240 * fade)) : IM_COL32(120, 200, 120, (int)(240 * fade)), 6.0f * k, 0, 1.5f);
  dl->AddText(font, fs, ImVec2(x, y + pad), IM_COL32(255, 255, 255, (int)(255 * fade)), N.text.c_str());
}

void tab_savestate() {
  const Theme& t = theme();
  ImGui::TextWrapped("Save the game as it is right now and come back to it, for example to take one photo again. There is one slot. It lives in memory "
                     "for this run only: it is not written to disk, a new save replaces it, and it is gone when you close the game. Your game saves and "
                     "album are not touched by a load, so a photo you kept after the state was saved stays in the album.");
  ImGui::Spacing();
  ImGui::TextColored(t.text_dim, "F5 saves the state, F8 loads it.");
  ImGui::Spacing();
  SnapStateInfo si{};
  snap_state_info(&si);
  if (si.busy) ImGui::BeginDisabled();
  if (ImGui::Button("Save state")) snap_state_request(1, kSlot);
  ImGui::SameLine();
  if (!si.has_state) ImGui::BeginDisabled();
  if (ImGui::Button("Load state")) snap_state_request(2, kSlot);
  if (!si.has_state) ImGui::EndDisabled();
  if (si.busy) ImGui::EndDisabled();
  ImGui::Spacing();
  if (si.busy) {
    ImGui::TextColored(t.text_dim, "Working...");
  } else if (si.has_state) {
    ImGui::TextColored(t.good, "A state is saved (this run).");
  } else {
    ImGui::TextColored(t.text_dim, "Nothing saved yet.");
  }
  if (si.done && !si.busy) {
    const bool bad = !si.last_ok;
    ImGui::TextColored(bad ? t.error : t.text_dim, "Last: %s", bad ? refusal_text(si.last_op, si.last_msg).c_str() : si.last_msg);
  }
}

}  // namespace snap::ui
