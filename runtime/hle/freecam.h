// SPDX-License-Identifier: MIT
// Free camera input (docs/freecam.md). Pure host code with no guest types, so the HLE tests build it with the rest of runtime/hle.
// Hub::poll (input_core.cpp) feeds every poll's raw and mapped input in; while the free camera is on the game sees an idle pad
// (the photo shutter and every other button are dead), and host/mods/freecam.cpp reads the accumulated movement once per frame.
#pragma once
#include <cstdint>
#include <string>

#include "input_core.h"

namespace hle::freecam {

// What the camera hook consumes once per frame.
struct Input {
  float move[3] = {0, 0, 0};       // right, up, forward: -1..1 (keys, left stick, up/down buttons, script)
  float look_px[2] = {0, 0};       // mouse pixels moved since the last consume (x right, y down)
  float look_stick[2] = {0, 0};    // right stick, -1..1 (x right, y up positive)
  float look_rate_deg[2] = {0, 0}; // script: degrees per second (yaw right positive, pitch up positive)
  float roll = 0;                  // -1..1 (Q / E, only used while the roll lock is off)
  float zoom = 0;                  // +1 zoom in (narrower view), -1 zoom out
  bool fast = false, slow = false;
  int speed_steps = 0;             // mouse wheel notches or d-pad Up/Down presses since the last consume
  int roll_lock_toggles = 0;
  int freeze_toggles = 0;
  int shots = 0;                   // clean-shot requests
  bool reset_view = false;         // back to the rail pose without leaving the free camera
  // script only (route tokens): a pose relative to the pose latched when the camera was switched on, and an absolute field of view
  bool pose_set = false;
  float pose[5] = {0, 0, 0, 0, 0}; // right, up, forward offsets in the rail camera's frame; yaw (right +) and pitch (up +) offsets in degrees
  bool fov_set = false;
  float fov_deg = 0;               // vertical field of view
  int freeze_set = -1;             // script: 1 freeze on, 0 off, -1 untouched
  int speed_set = -1;              // script: speed step to jump to
};

bool enabled();                       // SNAP_FREECAM (default on) or the F1 option: the hotkeys listen at all
void set_enabled(bool on);            // the F1 option (runtime/hle/settings.cpp)
bool active();                        // the free camera is on
void set_active(bool on, const char* why);   // the hotkeys, the script, and the camera hook (ride ended) call this
void note_rail(int present);          // host/mods/neofilm.cpp: a rail object updated this present
bool ride_active();                   // a rail updated within the last 30 presents
void set_release_hook(void (*fn)());   // host/mods/freecam.cpp: called when the camera goes off, to release the frozen world
bool wants_mouse();                   // Hub::poll: confine and recenter the cursor, report relative motion
void consume(Input* out);             // game thread, once per frame; zeroes the accumulators
bool take_enter();                    // true once after the camera was switched on (the hook latches the rail pose)
bool take_leave();                    // true once after it was switched off (the hook releases the clock)
// Hub::poll, after the mapped pad is known. `pad` and `touch` are replaced by an idle connected pad while the camera is on.
void poll(const hle::input::RawInput& raw, hle::input::PadSample& pad, hle::input::TouchSample& touch, bool menu_open, uint64_t now_ns);

// Route script tokens (SNAP_INPUT_SCRIPT, host/README.md "Scripted input"): fc=on|off|toggle, fcmove=<right>,<up>,<fwd>,
// fclook=<yaw>,<pitch> (degrees per second), fcpose=<right>,<up>,<fwd>,<yaw>,<pitch> (offsets from the rail pose), fcfov=<deg>,
// fcshot, fcfreeze=on|off, fcspeed=<step>. script_begin clears the per-poll script state, script_token adds one active token,
// script_end applies it. All three run on the poll thread.
bool script_valid_token(const std::string& t);
void script_begin();
void script_token(const std::string& t);
void script_end();
}  // namespace hle::freecam
