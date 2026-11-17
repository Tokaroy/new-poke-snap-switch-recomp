// SPDX-License-Identifier: MIT
// Scripted end-to-end test of the controls menu inside the real game window: SNAP_UI_TEST=<file>.
// One command per line, '#' starts a comment, times are seconds after the first present:
//   <t> key F1 [0|1]                post a real F1 key press to the game window (goes through the window procedure)
//   <t> tab <Bindings|Mouse|Controller|Accessibility|Test_input>
//   <t> rebind <A|B|...|Down> <kb|mouse|pad> <slot>     what a click on that slot does (starts the capture)
//   <t> reset_binds <all|kb|mouse|pad>                  the reset-to-defaults buttons without the confirmation question
//   <t> press <key:P|pad:east|mouse:left|...> <seconds> hold a source down through the input hub
//   <t> expect_button <A|ZL|...> <0|1>                  the bit the game reads from npad 0
//   <t> expect_preview <A|...> <0|1>                    the mapped state while the menu hides input from the game
//   <t> ui_lang <tag|auto>, i18n_collect <0|1>, expect_i18n   menu language override and "every text is translated" check (ui_i18n.cpp)
//   <t> expect_open <0|1> / expect_rebinding <0|1> / expect_tab <Name_with_underscores>
//   <t> wkey <Tab|Down|Up|Return|Space|Escape|PageUp|PageDown|A..Z> (posted WM_KEYDOWN/UP) / wclick_tab <Name_with_underscores> (posted mouse move + click on a tab)
//   <t> expect_bind <A|...> <kb|mouse|pad> <slot> <src|none>
//   <t> shot <file.bmp>             copy the window client area from the screen (needs a visible window)
//   <t> opt <key> <value>, expect_opt, expect_when, expect_pending <n>, expect_env <NAME> <value>   option pages (ui_options.cpp); "-" = empty value
//   <t> mods <toggle|move> <id> [offset], expect_mods <id:1,id:0>, apply_restart   Mods tab and the Apply and restart button
//   <t> quit                        print the summary and exit the process
// Every check logs "ui-test: ... PASS|FAIL"; the summary line is "ui-test: SUMMARY n checks, m failed".
#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_out.h"
#include "hle/hle.h"
#include "hle/input_core.h"
#include "hle/profiles.h"
#include "ui_i18n.h"
#include "ui_internal.h"
#include "ui_skin.h"

namespace snap::ui {

using namespace hle::input;

namespace {

struct Cmd {
  double at;
  std::string line;
  bool done = false;
};
struct Release {
  double at;
  Source s;
};
std::vector<Cmd> g_cmds;
std::vector<Release> g_rel;
std::map<std::string, std::string> g_rows_first;   // restart rows this script changed -> value before the first change (expect_pending rows)
bool g_loaded = false, g_active = false;
double g_t0 = 0;
int g_checks = 0, g_failed = 0;
std::string g_last_import;

void out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void out(const char* fmt, ...) {
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  hle::log("ui-test: %s", b);
  std::fprintf(stderr, "ui-test: %s\n", b);
  std::fflush(stderr);
}

void check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) ++g_failed;
  out("%s %s", what.c_str(), ok ? "PASS" : "FAIL");
}

Source parse_src(const std::string& s) {
  Source out_s;
  if (s != "none") parse_source(s, &out_s);
  return out_s;
}
bool parse_dev(const std::string& s, Device* d) {
  if (s == "kb") *d = kDevKeyboard;
  else if (s == "mouse") *d = kDevMouse;
  else if (s == "pad") *d = kDevController;
  else return false;
  return true;
}

void capture_window(std::string path) {
  HWND h = static_cast<HWND>(overlay_hwnd());
  if (!h) { out("shot: no window"); return; }
  std::thread([h, path]() {   // off the present thread: the swapchain must keep presenting while the screen is copied
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    Sleep(900);
    RECT cr;
    GetClientRect(h, &cr);
    POINT o = {0, 0};
    ClientToScreen(h, &o);
    int w = cr.right, ht = cr.bottom;
    HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateCompatibleBitmap(sdc, w, ht);
    SelectObject(mdc, bmp);
    BitBlt(mdc, 0, 0, w, ht, sdc, o.x, o.y, SRCCOPY);
    BITMAPINFOHEADER bi = {sizeof(bi), w, -ht, 1, 32, BI_RGB};
    std::vector<uint8_t> px((size_t)w * ht * 4);
    GetDIBits(mdc, bmp, 0, ht, px.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    BITMAPFILEHEADER fh = {0x4d42, (DWORD)(sizeof(fh) + sizeof(bi) + px.size()), 0, 0, sizeof(fh) + sizeof(bi)};
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(&fh, sizeof(fh), 1, f);
      std::fwrite(&bi, sizeof(bi), 1, f);
      std::fwrite(px.data(), 1, px.size(), f);
      std::fclose(f);
    }
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
    SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    out("shot: wrote %s (%dx%d)", path.c_str(), w, ht);
  }).detach();
}

double g_hold_until = 0;   // script time before which no further command runs (set by wclick_tab)
int g_hold_frames = 0;     // and the number of frames that must pass too (ImGui takes the move, the press and the release one frame each)

void run(const std::string& line, double t) {
  std::istringstream is(line);
  std::string cmd;
  is >> cmd;
  if (cmd == "key") {
    std::string k;
    int want = -1;   // optional: only press when the menu is not already in the wanted state (real input may interfere)
    is >> k >> want;
    HWND h = static_cast<HWND>(overlay_hwnd());
    if (want >= 0 && overlay_open() == (want != 0)) { out("t=%.1f menu already in wanted state, F1 skipped", t); }
    else if (k == "F1" && h) {
      PostMessageW(h, WM_KEYDOWN, VK_F1, 0x003b0001);
      PostMessageW(h, WM_KEYUP, VK_F1, (LPARAM)0xC03b0001);
      out("t=%.1f posted F1 to the game window", t);
    } else if (k == "F3" && h) {   // performance overlay hotkey (ui_fps.cpp)
      PostMessageW(h, WM_KEYDOWN, VK_F3, 0x003d0001);
      PostMessageW(h, WM_KEYUP, VK_F3, (LPARAM)0xC03d0001);
      out("t=%.1f posted F3 to the game window", t);
    }
  } else if (cmd == "wkey") {   // wkey <Tab|Down|Up|Left|Right|Return|Space|Escape|PageUp|PageDown|A..Z>: a real WM_KEYDOWN/UP to the game window (keyboard navigation of the menu)
    std::string k;
    is >> k;
    HWND h = static_cast<HWND>(overlay_hwnd());
    static const std::map<std::string, int> vks = {{"Tab", VK_TAB}, {"Down", VK_DOWN}, {"Up", VK_UP}, {"Left", VK_LEFT}, {"Right", VK_RIGHT}, {"Return", VK_RETURN}, {"Space", VK_SPACE},
                                                    {"Escape", VK_ESCAPE}, {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT}};
    int vk = k.size() == 1 && k[0] >= 'A' && k[0] <= 'Z' ? (int)k[0] : (vks.count(k) ? vks.at(k) : 0);
    check(h && vk, "wkey " + k);
    if (h && vk) {
      PostMessageW(h, WM_KEYDOWN, (WPARAM)vk, (LPARAM)(1 | (MapVirtualKeyW((UINT)vk, 0) << 16)));
      PostMessageW(h, WM_KEYUP, (WPARAM)vk, (LPARAM)(1 | (MapVirtualKeyW((UINT)vk, 0) << 16) | (3u << 30)));
    }
  } else if (cmd == "wclick_tab") {   // wclick_tab <Name_with_underscores>: a real mouse move, button down and up on that tab of the open menu
    std::string name;
    is >> name;
    for (auto& c : name) if (c == '_') c = ' ';
    HWND h = static_cast<HWND>(overlay_hwnd());
    float fx = 0, fy = 0;
    const bool ok = h && skin_tab_center(name.c_str(), &fx, &fy);
    check(ok, "wclick_tab " + name + (ok ? "" : " (tab not drawn)"));
    if (ok) { g_hold_until = t + 1.0; g_hold_frames = 8; out("t=%.1f wclick_tab %s at window pixel %.0f,%.0f", t, name.c_str(), fx, fy); }   // the click thread takes ~0.4 s: no later command (a check) may run before it is done, however late the frames are
    if (ok) std::thread([h, fx, fy]() {
      const LPARAM pos = MAKELPARAM((int)fx, (int)fy);
      // the real cursor is elsewhere, so Windows sends WM_MOUSELEAVE soon after a posted move and Dear ImGui then forgets the position: every button message gets its own move first
      PostMessageW(h, WM_MOUSEMOVE, 0, pos);
      Sleep(250);
      PostMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, pos);
      Sleep(120);
      PostMessageW(h, WM_LBUTTONUP, 0, pos);
    }).detach();
  } else if (cmd == "tab") {
    static std::string name;
    is >> name;
    for (auto& c : name) if (c == '_') c = ' ';
    menu_select_tab(name.c_str());
  } else if (cmd == "rebind") {
    std::string a, d;
    int slot = 0;
    is >> a >> d >> slot;
    Device dev;
    int act = button_from_name(a);
    check(act >= 0 && act <= kDown && parse_dev(d, &dev) && menu_begin_rebind(act, dev, slot), "rebind " + a + " " + d + " " + std::to_string(slot) + " started");
  } else if (cmd == "reset_binds") {   // reset_binds <all|kb|mouse|pad>: what the reset buttons do after their confirmation
    std::string d;
    is >> d;
    Device dev = kDevKeyboard;
    const bool all = d == "all";
    check((all || parse_dev(d, &dev)) && menu_reset_binds(all ? (int)kDevCount : (int)dev), "reset_binds " + d);
  } else if (cmd == "press") {
    std::string src;
    double secs = 0.3;
    is >> src >> secs;
    Source s = parse_src(src);
    check(s.kind != Source::None, "press " + src);
    hub().inject_source(s, true);
    g_rel.push_back({t + secs, s});
  } else if (cmd == "expect_button" || cmd == "expect_preview") {
    std::string n;
    int want = 1;
    is >> n >> want;
    int b = button_from_name(n);
    hub().poll(0);
    PadSample p = cmd == "expect_button" ? hub().pad() : hub().preview();
    bool down = b >= 0 && ((p.buttons >> b) & 1);
    check(b >= 0 && down == (want != 0), cmd + " " + n + " == " + std::to_string(want) + " (buttons=" + std::to_string((unsigned long long)p.buttons) + ")");
  } else if (cmd == "vol" || cmd == "expect_vol") {   // vol <master|bgm|se|voice> <percent> ; expect_vol checks snap_audio_get_volume
    std::string c;
    float pct = 0;
    is >> c >> pct;
    int cat = c == "master" ? SNAP_VOL_MASTER : c == "bgm" ? SNAP_VOL_BGM : c == "se" ? SNAP_VOL_SE : SNAP_VOL_VOICE;
    if (cmd == "vol") { snap_audio_set_volume(cat, pct / 100.0f); out("t=%.1f set volume %s = %.0f%%", t, c.c_str(), pct); }
    else check(std::fabs(snap_audio_get_volume(cat) - pct / 100.0f) < 0.006f, "volume " + c + " == " + std::to_string((int)pct) + "% (got " + std::to_string((int)std::lround(snap_audio_get_volume(cat) * 100.0f)) + "%)");
  } else if (cmd == "opt") {   // opt <key> <value|-> : what changing the control does ("-" = empty value)
    std::string k, v;
    is >> k >> v;
    if (v == "-") v.clear();
    std::string when;
    if (options_test_option_when(k, &when) && when == "restart" && !g_rows_first.count(k)) g_rows_first[k] = options_test_get(k);
    check(options_test_set(k, v), "set option " + k + " = '" + v + "'");
  } else if (cmd == "expect_opt") {   // expect_opt <key> <value|->
    std::string k, v;
    is >> k >> v;
    if (v == "-") v.clear();
    const std::string got = options_test_get(k);
    check(got == v, "option " + k + " == '" + v + "' (is '" + got + "')");
  } else if (cmd == "scroll") {   // scroll <0..1>: the option list of the open page
    float f = 0;
    is >> f;
    options_test_scroll(f);
  } else if (cmd == "expect_when") {   // expect_when <key> <live|restart|soon>
    std::string k, w, got;
    is >> k >> w;
    check(options_test_option_when(k, &got) && got == w, "option " + k + " is " + w + " (is '" + got + "')");
  } else if (cmd == "expect_pending") {   // expect_pending <n>: settings that wait for a restart
    std::string arg;
    is >> arg;
    std::string names;
    const int got = options_pending_restart(&names);
    int n = 0;
    if (arg == "rows") {   // expect_pending rows: count the restart rows this script changed (value now != value before its first change), so rows added by other work do not move the number
      for (const auto& kv : g_rows_first) if (options_test_get(kv.first) != kv.second) ++n;
      if (names.find("Mods") != std::string::npos) ++n;   // the mods order has its own check (expect_mods)
    } else n = std::atoi(arg.c_str());
    check(got == n, "pending restart options == " + std::to_string(n) + " (is " + std::to_string(got) + ": " + names + ")");
  } else if (cmd == "expect_env") {   // expect_env <NAME> <value|->: the env var the game reads at start
    std::string n, v;
    is >> n >> v;
    const char* e = std::getenv(n.c_str());
    if (v == "-") v.clear();
    check(std::string(e ? e : "") == v, "env " + n + " == '" + v + "' (is '" + (e ? e : "") + "')");
  } else if (cmd == "mods") {   // mods <toggle|move> <pack id> [offset]: what the Mods tab buttons do (writes mods.json)
    std::string op, id, state;
    int arg = 0;
    is >> op >> id >> arg;
    check(options_test_mods(op, id, arg, &state), "mods " + op + " " + id + " " + std::to_string(arg));
  } else if (cmd == "expect_mods") {   // expect_mods <id:1,id:0,...> : packs in priority order with enabled flags
    std::string want, state;
    is >> want;
    options_test_mods("read", "", 0, &state);
    check(state == want, "mods state == " + want + " (is " + state + ")");
  } else if (cmd == "apply_restart") {   // the "Apply and restart" button
    out("t=%.1f apply and restart (the game restarts)", t);
    std::fflush(stderr);
    restart_game();
  } else if (cmd == "expect_open") {
    int want = 1;
    is >> want;
    check(overlay_open() == (want != 0), "menu open == " + std::to_string(want));
  } else if (cmd == "expect_tab") {   // the page the menu shows (names as in the tab strip; underscores stand for spaces)
    std::string want;
    is >> want;
    for (char& ch : want) if (ch == '_') ch = ' ';
    check(want == menu_current_tab(), "tab == " + want + " (is " + menu_current_tab() + ")");
  } else if (cmd == "expect_focus") {   // expect_focus <0|1>: the cursor is on an item of the page (after a d-pad move)
    int want = 1;
    is >> want;
    check(menu_page_cursor() == (want != 0), "page cursor == " + std::to_string(want) + " (" + menu_nav_debug() + ")");
  } else if (cmd == "ui_lang") {   // ui_lang <ja|de|...|auto>: draw the menu in that language until changed (the setting itself is not touched)
    std::string tag;
    is >> tag;
    check(i18n_force(tag == "auto" ? std::string() : tag), "menu language " + tag);
  } else if (cmd == "i18n_collect") {   // i18n_collect <0|1>: remember every text the menu asks tr() for (use before visiting the pages)
    int on = 1;
    is >> on;
    i18n_collect(on != 0);
    if (on) i18n_collect_reset();
  } else if (cmd == "expect_i18n") {   // every text asked for since i18n_collect 1 has a line in every language
    std::string rep;
    const size_t bad = i18n_check(&rep);
    for (size_t p = 0; p < rep.size();) {   // one log line per problem (out() has a small buffer)
      const size_t q = rep.find('\n', p);
      out("i18n: %s", rep.substr(p, q == std::string::npos ? std::string::npos : q - p).c_str());
      p = q == std::string::npos ? rep.size() : q + 1;
    }
    check(bad == 0, "menu texts translated (" + std::to_string(bad) + " problems)");
  } else if (cmd == "expect_rebinding") {
    int want = 1;
    is >> want;
    check(menu_rebinding() == (want != 0), "rebinding == " + std::to_string(want));
  } else if (cmd == "expect_bind") {
    std::string a, d, src;
    int slot = 0;
    is >> a >> d >> slot >> src;
    Device dev;
    int act = button_from_name(a);
    bool ok = act >= 0 && parse_dev(d, &dev);
    Source got = ok ? slot_get(action_sources(hub().mapping(), act), dev, slot) : Source{};
    check(ok && got == parse_src(src), "binding " + a + " " + d + " " + std::to_string(slot) + " == " + src + " (is '" + source_to_string(got) + "')");
  } else if (cmd == "saves_import") {   // saves_import <fixture folder name>: what "Import test save" does; remembers the new profile name
    std::string fx, err;
    is >> fx;
    static std::string last_import;
    check(saves_test_import(fx, &last_import, &err), "import test save " + fx);
    if (!err.empty()) out("  (%s)", err.c_str());
    g_last_import = last_import;
  } else if (cmd == "saves_switch") {    // saves_switch: "Switch to this profile" on the profile that was just imported; relaunches the game
    out("t=%.1f switching to profile \"%s\" (the game restarts)", t, g_last_import.c_str());
    std::fflush(stderr);
    saves_test_switch(g_last_import);
  } else if (cmd == "saves_backups") {   // saves_backups open <n> | queue | queued: the F1 > Saves "Restore a backup" list (open = show it with row n picked)
    std::string a, what;
    int n = 0;
    is >> a >> n;
    const bool ok = saves_test_backups(a, n, &what);
    check(ok, "saves_backups " + a + (what.empty() ? "" : " (" + what + ")"));
  } else if (cmd == "expect_profile") {   // expect_profile <name>: the running game's profile
    std::string n;
    std::getline(is >> std::ws, n);
    check(hle::profiles::active().on && hle::profiles::active().name == n, "profile is " + n);
  } else if (cmd == "save") {
    std::string err;
    check(hub().save(&err), "save " + err);
  } else if (cmd == "shot") {
    std::string p;
    is >> p;
    capture_window(p);
  } else if (cmd == "quit") {
    out("SUMMARY %d checks, %d failed", g_checks, g_failed);
    std::fflush(stderr);
    ExitProcess(g_failed ? 1 : 0);
  } else {
    out("unknown command '%s'", line.c_str());
  }
}

void load() {
  g_loaded = true;
  const char* path = std::getenv("SNAP_UI_TEST");
  if (!path || !*path) return;
  std::ifstream f(path);
  if (!f) { std::fprintf(stderr, "ui-test: cannot open %s\n", path); return; }
  std::string line;
  while (std::getline(f, line)) {
    size_t h = line.find('#');
    if (h != std::string::npos) line.resize(h);
    std::istringstream is(line);
    double at;
    if (!(is >> at)) continue;
    std::string rest;
    std::getline(is, rest);
    g_cmds.push_back({at, rest});
  }
  g_active = !g_cmds.empty();
  std::fprintf(stderr, "ui-test: %zu commands from %s\n", g_cmds.size(), path);
}

// true unless `line` is an expect_tab / expect_open command whose condition does not hold yet
bool expectation_holds(const std::string& line) {
  std::istringstream is(line);
  std::string cmd;
  is >> cmd;
  if (cmd == "expect_open") { int want = 1; is >> want; return overlay_open() == (want != 0); }
  if (cmd == "expect_tab") { std::string want; is >> want; for (char& ch : want) if (ch == '_') ch = ' '; return want == menu_current_tab(); }
  return true;
}

}  // namespace

bool test_active() { return g_active; }

void test_tick(double now) {
  if (!g_loaded) { load(); g_t0 = now; }
  if (!g_active) return;
  const double t = now - g_t0;
  for (size_t i = 0; i < g_rel.size();) {
    if (t >= g_rel[i].at) { hub().inject_source(g_rel[i].s, false); g_rel.erase(g_rel.begin() + (long)i); } else ++i;
  }
  for (auto& c : g_cmds) {
    if (c.done || t < c.at) continue;
    if (t < g_hold_until || g_hold_frames > 0) {
      if (g_hold_frames > 0 && --g_hold_frames == 0) { const ImGuiIO& io = ImGui::GetIO(); out("t=%.1f after the click: imgui mouse %.0f,%.0f down=%d display %.0fx%.0f", t, io.MousePos.x, io.MousePos.y, (int)io.MouseDown[0], io.DisplaySize.x, io.DisplaySize.y); }
      break;
    }
    if (t < c.at + 6.0 && !expectation_holds(c.line)) break;   // expect_tab / expect_open: the menu reacts to a posted key or click a frame or two later (a slow frame rate more): wait up to 6 s, then run it as the failing check
    c.done = true;
    run(c.line, t);
    break;   // one command per frame: the menu and the input hub get a frame to react
  }
}

}  // namespace snap::ui
