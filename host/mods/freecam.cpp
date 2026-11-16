// SPDX-License-Identifier: MIT
// Free camera (docs/freecam.md): fly the ride camera anywhere while the NEO-ONE waits on its rail.
//
// Input comes from runtime/hle/freecam.cpp (hotkeys, controller chord, route tokens); this file owns the camera and the pause:
//  - camera: the ride camera builder (main+0x295170) puts eye / target / up at obj+0xd0 / 0xe0 / 0xf0 and derives the view basis with
//    0x294a50, then calls tanf for the projection (main+0x296400, obj = x20). h_tanf (lodanim.cpp) calls snap_freecam_camera there; it
//    replaces the three vectors, re-runs 0x294a50 so the view data matches, and returns the field of view and far plane to use. The game's
//    own camera state is never written, so switching the camera off puts the rail camera back exactly.
//  - rail: rt_gm_neo_speed (neofilm.cpp) calls snap_freecam_rail after the rail update; while the camera is on the progress is put back
//    to what it was, so the NEO-ONE stands still and events keyed to its position do not fire. Everything keyed to the clock keeps running.
//  - "Freeze world" sets the game clock's stop bytes (the same ones a pause sets), which turns the frame delta to zero for animation.
//  - the photo shutter is dead because Hub::poll hands the game an idle pad while the camera is on (runtime/hle/freecam.cpp).
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hle/freecam.h"
#include "loader.h"

extern "C" uint64_t snap_present_count();   // host/boot.cpp
extern "C" void nvnHostFreecamShot(void);   // runtime/nvn/nvn_freecam.cpp: save the next scene (no HUD) as a PNG

namespace {
const uint64_t kMainBase = 0x7100000000ull;
const uint64_t kBuildView = kMainBase + 0x294a50;   // camera view data from eye / target / up at obj+0xd0..0xff
const uint64_t kClockPtr = 0x7100cfc000ull + 0x238;  // *kClockPtr = the game clock object; stop bytes at +0x24 / +0x3c / +0x54

struct V3 { float x, y, z; };
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float len(V3 a) { return std::sqrt(dot(a, a)); }
V3 norm(V3 a) { float l = len(a); return l > 1e-12f ? a * (1.0f / l) : V3{0, 0, 1}; }
bool finite(V3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
const float kDeg = 3.14159265358979f / 180.0f;
const V3 kWorldUp = {0, 1, 0};
// right-handed lookat: right = forward x up. If a test shows the view mirrored, flip this.
const float kHand = 1.0f;

V3 right_of(V3 f) { return norm(cross(f, kWorldUp)) * kHand; }

bool g_log = getenv("SNAP_FREECAM_LOG") && getenv("SNAP_FREECAM_LOG")[0] != '0';

struct State {
  bool on = false;
  uint64_t obj = 0;
  uint64_t present = ~0ull;      // last present the pose was integrated for
  double last_s = 0;
  // the rail camera when the free camera was switched on (script poses are offsets from it)
  V3 e0{}, f0{}, r0{}, u0{};
  float half0 = 0.4f, far0 = 1000.0f, yaw0 = 0, pitch0 = 0;
  // the free camera
  V3 pos{}, fh{0, 0, 1};         // position, horizontal heading (unit, y = 0)
  float pitch = 0, roll = 0, half = 0.4f;
  bool roll_lock = true;
  int speed = 3;
  float far_now = 1000.0f;
  // the pose the current frame is built with (re-applied when the builder runs again within one present)
  V3 eye{}, target{}, up{};
  // world freeze
  bool frozen = false, have_stop = false;
  uint8_t stop_prev[3] = {0, 0, 0};
  // rail freeze
  int frames = 0;
} S;

// scene scale: world units per second at speed step 0..7 (the Florio rail moves about 2.5 units a second)
const float kSpeeds[] = {0.4f, 1.0f, 2.5f, 6.0f, 15.0f, 40.0f, 100.0f, 250.0f};
const int kNSpeeds = (int)(sizeof kSpeeds / sizeof kSpeeds[0]);

double now_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}
bool readable(const void *p, size_t n) { return !IsBadReadPtr(p, n); }

uint8_t *clock_stop_bytes() {
  if (!readable((void *)kClockPtr, 8)) return nullptr;
  const uint64_t clk = *(const uint64_t *)kClockPtr;
  if (!clk || !readable((void *)(clk + 0x24), 0x34)) return nullptr;
  return (uint8_t *)clk;
}
void set_freeze(bool on) {
  uint8_t *clk = clock_stop_bytes();
  if (!clk) return;
  static const int off[3] = {0x24, 0x3c, 0x54};
  if (on) {
    if (!S.have_stop) { for (int i = 0; i < 3; i++) S.stop_prev[i] = clk[off[i]]; S.have_stop = true; }
    for (int i = 0; i < 3; i++) clk[off[i]] = 1;
  } else if (S.have_stop) {
    for (int i = 0; i < 3; i++) clk[off[i]] = S.stop_prev[i];
    S.have_stop = false;
  }
}

void leave() {
  if (S.on && S.obj && readable((void *)(S.obj + 0x124), 4)) *(float *)(S.obj + 0x124) = S.far0;
  if (S.frozen || S.have_stop) set_freeze(false);
  S.frozen = false;
  S.on = false;
  S.obj = 0;
}

// camera yaw / pitch from a forward vector
void set_heading(V3 f) {
  S.pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
  V3 h = {f.x, 0, f.z};
  S.fh = len(h) > 1e-6f ? norm(h) : S.fh;
}
V3 forward() { const float cp = std::cos(S.pitch); return {S.fh.x * cp, std::sin(S.pitch), S.fh.z * cp}; }
// turn the heading by `a` radians toward the camera's right
void yaw_by(float a) {
  const V3 rh = right_of(S.fh);
  S.fh = norm(S.fh * std::cos(a) + rh * std::sin(a));
}

void apply_input(const hle::freecam::Input &in, float dt, V3 rail_eye, V3 rail_target, V3 rail_up) {
  if (in.speed_set >= 0) S.speed = std::clamp(in.speed_set, 0, kNSpeeds - 1);
  S.speed = std::clamp(S.speed + in.speed_steps, 0, kNSpeeds - 1);
  if (in.roll_lock_toggles & 1) { S.roll_lock = !S.roll_lock; if (S.roll_lock) S.roll = 0; }
  if (in.freeze_toggles & 1) S.frozen = !S.frozen;
  if (in.freeze_set >= 0) S.frozen = in.freeze_set != 0;
  if (in.reset_view) {   // back to the rail pose, still free
    S.pos = rail_eye;
    set_heading(norm(rail_target - rail_eye));
    S.roll = 0;
    S.half = S.half0;
    (void)rail_up;
  }
  // look: mouse pixels (a full screen width is about 90 degrees at the rail field of view), right stick, route rate
  const float fovk = S.half / S.half0;   // narrower view, slower look
  const float px_deg = 0.10f * fovk;
  float dyaw = in.look_px[0] * px_deg * kDeg + in.look_stick[0] * 110.0f * kDeg * dt * fovk + in.look_rate_deg[0] * kDeg * dt;
  float dpit = -in.look_px[1] * px_deg * kDeg + in.look_stick[1] * 80.0f * kDeg * dt * fovk + in.look_rate_deg[1] * kDeg * dt;
  yaw_by(dyaw);
  S.pitch = std::clamp(S.pitch + dpit, -89.0f * kDeg, 89.0f * kDeg);
  if (!S.roll_lock) S.roll += in.roll * 60.0f * kDeg * dt;
  // fly: along the view for forward, world up for up, the view's right for strafing
  const float sp = kSpeeds[S.speed] * (in.fast ? 4.0f : 1.0f) * (in.slow ? 0.2f : 1.0f);
  const V3 f = forward(), r = right_of(f);
  S.pos = S.pos + (r * in.move[0] + kWorldUp * in.move[1] + f * in.move[2]) * (sp * dt);
  // zoom: exponential, one e-fold per second at full deflection
  if (in.zoom != 0) S.half = std::clamp(S.half * std::exp(-in.zoom * 1.0f * dt), 1.0f * kDeg, 80.0f * kDeg);
  if (in.fov_set) S.half = std::clamp(in.fov_deg * 0.5f * kDeg, 1.0f * kDeg, 80.0f * kDeg);
  if (in.pose_set) {   // route: an exact pose relative to the rail camera at switch-on
    S.pos = S.e0 + S.r0 * in.pose[0] + S.u0 * in.pose[1] + S.f0 * in.pose[2];
    set_heading(S.f0);
    yaw_by(in.pose[3] * kDeg);
    S.pitch = std::clamp(S.pitch + in.pose[4] * kDeg, -89.0f * kDeg, 89.0f * kDeg);
  }
  if (in.shots > 0) nvnHostFreecamShot();
}
}  // namespace

// Registered with hle::freecam (set_release_hook): runs on the poll thread whenever the free camera goes off, so the world is never left
// frozen when the camera hook is not called any more (the ride ended).
static void release_clock() { if (S.have_stop) set_freeze(false); }

// Called by rt_gm_neo_speed (neofilm.cpp) after every rail update: R = the rail object, progress float at R+0x70.
extern "C" void snap_freecam_rail(uint64_t R) {
  static uint64_t rail_obj[8]; static float rail_prog[8]; static int n;
  if (!R || !readable((void *)(R + 0x70), 4)) return;
  hle::freecam::note_rail((int)snap_present_count());
  int k = -1;
  for (int i = 0; i < n; i++) if (rail_obj[i] == R) { k = i; break; }
  if (k < 0) { if (n >= 8) return; k = n++; rail_obj[k] = R; rail_prog[k] = *(float *)(R + 0x70); }
  float *p = (float *)(R + 0x70);
  if (hle::freecam::active()) *p = rail_prog[k];   // the NEO-ONE waits
  else rail_prog[k] = *p;
}

// Called by h_tanf (lodanim.cpp) for every tanf call. Returns true when it changed the camera; *half = the half field of view in radians
// to use (in: what the game asked for), *far_ = the far plane the projection uses (in: the game's).
extern "C" bool snap_freecam_camera(Cpu *c, float *half, float *far_) {
  if (((c->x[30] - kMainBase) & 0xffffffffull) != 0x296400) return false;
  { static bool reg = false; if (!reg) { reg = true; hle::freecam::set_release_hook(release_clock); } }
  const bool want = hle::freecam::active();
  if (!want && !S.on) return false;
  const uint64_t obj = c->x[20];
  if (!obj || !readable((void *)(obj + 0xd0), 0x30) || !readable((void *)(obj + 0x120), 12)) return false;
  V3 *eye = (V3 *)(obj + 0xd0), *tgt = (V3 *)(obj + 0xe0), *up = (V3 *)(obj + 0xf0);
  const uint64_t present = snap_present_count();
  if (!want) { leave(); return false; }
  if (!hle::freecam::ride_active()) { hle::freecam::set_active(false, "the ride ended"); leave(); return false; }
  if (!S.on) {
    // switch on: latch the rail camera as it is this frame
    if (!hle::freecam::take_enter()) return false;
    S = State{};
    S.on = true;
    S.obj = obj;
    S.e0 = *eye;
    S.f0 = norm(*tgt - *eye);
    S.r0 = right_of(S.f0);
    S.u0 = norm(cross(S.r0, S.f0));
    S.half0 = S.half = *half;
    S.far0 = S.far_now = *(float *)(obj + 0x124);
    S.pos = *eye;
    set_heading(S.f0);
    { static const bool fz = getenv("SNAP_FREECAM_FREEZE") && getenv("SNAP_FREECAM_FREEZE")[0] == '1'; S.frozen = fz; }
    S.present = ~0ull;
    S.last_s = now_s();
    hle::freecam::Input dummy;
    hle::freecam::consume(&dummy);
    fprintf(stderr, "[freecam] rail camera latched: eye (%.2f %.2f %.2f) target (%.2f %.2f %.2f) up (%.2f %.2f %.2f) half-fov %.1f deg near %.3f far %.1f obj %#llx\n",
            eye->x, eye->y, eye->z, tgt->x, tgt->y, tgt->z, up->x, up->y, up->z, *half / kDeg, *(float *)(obj + 0x120), S.far0, (unsigned long long)obj);
  }
  if (obj != S.obj) return false;   // another camera object built this frame (not the ride camera)
  const V3 rail_eye = *eye, rail_tgt = *tgt, rail_up = *up;
  if (present != S.present) {
    S.present = present;
    const double t = now_s();
    const float dt = (float)std::clamp(t - S.last_s, 0.0, 0.1);
    S.last_s = t;
    hle::freecam::Input in;
    hle::freecam::consume(&in);
    apply_input(in, dt, rail_eye, rail_tgt, rail_up);
    set_freeze(S.frozen);
    if (S.frozen) hle::freecam::note_rail((int)present);   // a frozen clock stops the rail updates: the camera hook itself keeps the ride "running"
    // the pose for this frame
    const V3 f = forward();
    const V3 r = right_of(f);
    const V3 u0 = norm(cross(r, f));
    const float cr = std::cos(S.roll), sr = std::sin(S.roll);
    S.eye = S.pos;
    S.target = S.pos + f;
    S.up = u0 * cr + r * sr;
    if (!finite(S.eye) || !finite(S.target) || !finite(S.up)) { S.pos = rail_eye; set_heading(S.f0); S.roll = 0; S.eye = rail_eye; S.target = rail_tgt; S.up = rail_up; }
    // a wider far plane the further the camera is from the rail: the course streams in and out around the rail, so keep the sky and
    // far scenery inside the frustum
    const float dist = len(S.pos - S.e0);
    S.far_now = S.far0 + dist * 2.0f;
    if (g_log && (++S.frames % 60) == 1)
      fprintf(stderr, "[freecam] present %llu pos (%.2f %.2f %.2f) heading (%.2f %.2f) pitch %.1f roll %.1f half-fov %.1f speed %d far %.0f %s\n", (unsigned long long)present,
              S.pos.x, S.pos.y, S.pos.z, S.fh.x, S.fh.z, S.pitch / kDeg, S.roll / kDeg, S.half / kDeg, S.speed, S.far_now, S.frozen ? "frozen" : "");
  }
  *eye = S.eye; *tgt = S.target; *up = S.up;
  *(float *)(obj + 0x124) = S.far_now;
  loader::call_guest_nested(c, kBuildView, {obj});
  *half = S.half;
  if (far_) *far_ = S.far_now;
  return true;
}
