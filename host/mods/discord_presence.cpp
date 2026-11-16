// SPDX-License-Identifier: MIT
// Discord Rich Presence (project code, ours; no Discord SDK, no Discord binary). Off unless SNAP_DISCORD_PRESENCE=1 or the discord_presence setting.
//
// Discord's desktop app listens on a local named pipe, \\.\pipe\discord-ipc-0 .. 9. We open it, send a handshake frame (opcode 0, {"v":1,"client_id":..}),
// then SET_ACTIVITY frames (opcode 1) with what the player is doing. A frame is a little-endian int32 opcode, an int32 length and a UTF-8 JSON payload.
// Nothing is sent anywhere else; with Discord closed the thread only retries the pipe every few seconds. The activity holds only the course name,
// the mode and the time the session started: no user name, no paths.
//
// What the player is doing comes from the files the game opens (runtime/hle/fs.cpp calls snap_presence_note_open for every successful OpenFile):
// nx/Pack/LevelComposite/master/LC_<id>_*.drp is the course pack that is loaded. LC_001 .. LC_011 are the ride courses, LC_101 and LC_103 the
// Research Camp lab. The course name comes from the player's own extracted English text (album.cpp course_label), never shipped.
//
// App id: Discord needs an application id from the developer portal (a public number, not a secret). SNAP_DISCORD_APP_ID or the discord_app_id
// setting override kDefaultAppId; with neither set the feature stays idle and says so once in the log.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#if defined(__has_include) && __has_include("../album.h")
#include "../album.h"
#define SNAP_HAVE_ALBUM 1   // the Album page's text reader gives the course names; without it the presence shows the mode only
#endif
#include "../../runtime/hle/hle_config.h"
#include "../../runtime/hle/settings.h"

namespace {

// Registered at discord.com/developers by the project owner; empty until then (see docs/player-options.md).
constexpr const char* kDefaultAppId = "";

struct Lc { int lc; int course; };   // pack id (LC_<id>) -> research course 1..30 for the name; 0 = unknown (the mode still shows)
constexpr Lc kLc[] = {
    {1, 1},
};

std::mutex g_mu;
int g_lc = 0;                 // last ride pack id seen, 0 = none yet
bool g_camp = false;          // last pack seen was the Research Camp lab
std::atomic<bool> g_started{false};
std::string g_app_id_logged;

std::string env(const char* n) { const char* e = std::getenv(n); return e && *e ? e : ""; }
bool truthy(const std::string& s) { return s == "1" || s == "on" || s == "true" || s == "yes"; }

bool enabled() {
  const std::string e = env("SNAP_DISCORD_PRESENCE");
  if (!e.empty()) return truthy(e);
  return truthy(hle::settings::get("discord_presence", "0"));
}
std::string app_id() {
  std::string e = env("SNAP_DISCORD_APP_ID");
  if (e.empty()) e = hle::settings::get("discord_app_id", "");
  return e.empty() ? std::string(kDefaultAppId) : e;
}

std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  return o + "\"";
}

struct Pipe {
  HANDLE h = INVALID_HANDLE_VALUE;
  bool open() {
    for (int i = 0; i < 10; ++i) {
      char name[64];
      std::snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
      h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      if (h != INVALID_HANDLE_VALUE) return true;
    }
    return false;
  }
  void close() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); h = INVALID_HANDLE_VALUE; }
  bool send(uint32_t op, const std::string& json) {
    std::string f(8, '\0');
    std::memcpy(&f[0], &op, 4);
    const uint32_t n = (uint32_t)json.size();
    std::memcpy(&f[4], &n, 4);
    f += json;
    DWORD w = 0;
    return WriteFile(h, f.data(), (DWORD)f.size(), &w, nullptr) && w == f.size();
  }
  // Reads and drops every frame that is waiting (Discord answers each SET_ACTIVITY; an unread pipe would fill up). Returns false when the pipe is gone.
  // With `wait_ms` it waits that long for the first frame (the handshake answer) and returns its opcode in *op.
  bool drain(int wait_ms, uint32_t* op = nullptr, std::string* body = nullptr) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
      DWORD avail = 0;
      if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return false;
      if (avail >= 8) {
        uint32_t hdr[2] = {};
        DWORD r = 0;
        if (!ReadFile(h, hdr, 8, &r, nullptr) || r != 8) return false;
        std::string b(hdr[1] < (1u << 20) ? hdr[1] : 0, '\0');
        DWORD got = 0;
        while (got < b.size()) { DWORD g = 0; if (!ReadFile(h, &b[got], (DWORD)(b.size() - got), &g, nullptr) || !g) return false; got += g; }
        if (op) *op = hdr[0];
        if (body) *body = b;
        if (hdr[0] == 2) return false;   // CLOSE
        if (!wait_ms) continue;
        return true;
      }
      if (wait_ms <= 0 || std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() >= wait_ms) return wait_ms <= 0;
      Sleep(20);
    }
  }
};

void describe(std::string* details, std::string* state) {
  int lc;
  bool camp;
  { std::lock_guard<std::mutex> g(g_mu); lc = g_lc; camp = g_camp; }
  if (camp) { *details = "At the Research Camp"; *state = "Photo evaluation and lab"; return; }
  if (!lc) { *details = "In the menus"; *state = "Snap Recomp"; return; }
  int course = 0;
  for (const Lc& e : kLc) if (e.lc == lc) course = e.course;
  std::string label;
#ifdef SNAP_HAVE_ALBUM
  if (course) label = album::course_label(hle::config().romfs_root, course);
#endif
  *details = label.empty() ? "Riding a course" : label;
  *state = "Taking photos";
}

std::string activity_json(bool clear, long long started_unix) {
  std::string nonce = std::to_string(GetTickCount64());
  std::string act = "null";
  if (!clear) {
    std::string details, state;
    describe(&details, &state);
    act = "{\"details\":" + json_str(details) + ",\"state\":" + json_str(state) + ",\"timestamps\":{\"start\":" + std::to_string(started_unix) + "}}";
  }
  return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) + ",\"activity\":" + act + "},\"nonce\":\"" + nonce + "\"}";
}

void run() {
  const long long started = (long long)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  Pipe p;
  bool shown = false;       // presence is currently set on the connected pipe
  std::string last;         // the last activity sent (details|state), to send only changes
  for (;;) {
    const bool on = enabled();
    if (!on) {
      if (p.h != INVALID_HANDLE_VALUE) {
        if (shown) { p.send(1, activity_json(true, started)); p.drain(0); }
        p.close();
        shown = false;
        last.clear();
      }
      Sleep(3000);
      continue;
    }
    const std::string id = app_id();
    if (id.empty()) {
      if (g_app_id_logged != "none") { std::fprintf(stderr, "discord: presence is on but no application id is set (SNAP_DISCORD_APP_ID); idle\n"); g_app_id_logged = "none"; }
      Sleep(3000);
      continue;
    }
    if (p.h == INVALID_HANDLE_VALUE) {
      shown = false;
      last.clear();
      if (!p.open()) { Sleep(5000); continue; }   // Discord is not running; try again later
      if (!p.send(0, "{\"v\":1,\"client_id\":" + json_str(id) + "}")) { p.close(); Sleep(5000); continue; }
      uint32_t op = 0;
      std::string body;
      if (!p.drain(3000, &op, &body) || body.find("READY") == std::string::npos) {
        std::fprintf(stderr, "discord: handshake not accepted (%s)\n", body.substr(0, 120).c_str());
        p.close();
        Sleep(10000);
        continue;
      }
      std::fprintf(stderr, "discord: connected\n");
    }
    std::string d, s;
    describe(&d, &s);
    const std::string cur = d + "|" + s;
    if (cur != last) {
      if (!p.send(1, activity_json(false, started))) { p.close(); continue; }
      last = cur;
      shown = true;
    }
    if (!p.drain(0)) { p.close(); continue; }
    Sleep(2000);
  }
}

}  // namespace

extern void (*g_snap_presence_open)(const char* path);   // runtime/hle/fs.cpp: called for every successful nn::fs::OpenFile

// Cheap: one substring test per opened file.
static void note_open(const char* path) {
  if (!g_started.exchange(true)) std::thread(run).detach();   // sleeps 3 s between checks while presence is off
  const char* lc = std::strstr(path, "LevelComposite/master/LC_");
  if (!lc) lc = std::strstr(path, "LevelComposite\\master\\LC_");
  if (!lc) return;
  const int id = std::atoi(lc + std::strlen("LevelComposite/master/LC_"));
  std::lock_guard<std::mutex> g(g_mu);
  if (id >= 1 && id <= 99) { g_lc = id; g_camp = false; }
  else if (id == 101 || id == 103) g_camp = true;
}

static const int g_registered = (g_snap_presence_open = &note_open, 0);
