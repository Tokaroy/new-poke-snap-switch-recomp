# Free camera

Fly the ride camera anywhere during a ride, take clean shots, and go back to the rail. Source: `runtime/hle/freecam.cpp` (input), `host/mods/freecam.cpp` (camera and pause), `runtime/nvn/nvn_freecam.cpp` (clean PNG).

## Using it

During a ride press **F6** (or click both sticks, **L3 + R3**). The NEO-ONE stops on its rail and the camera is yours. Press F6 (or L3 + R3) again: the camera returns to the rail exactly where the ride paused and the ride goes on. The free camera switches itself off when the ride ends. It does nothing outside a ride (hub, lab, menus, photo select).

| Action | Keyboard and mouse | Controller |
| --- | --- | --- |
| Move | W A S D (forward follows where you look) | Left stick |
| Look | Mouse (the cursor is held at the window center), arrow keys | Right stick |
| Up / down | Space / C or Left Ctrl | D-pad up / down |
| Faster / slower | Mouse wheel changes the speed step (8 steps), Left Shift is 4x, Left Alt is 0.2x | L / R change the step, L3 held is 4x, R3 held is 0.2x |
| Zoom (field of view) | Z in, X out | ZL in, ZR out |
| Roll | Q / E, only while the roll lock is off | |
| Roll lock (default on) | G | |
| Back to the rail pose, still free | Home | B |
| Freeze world on / off | F7 | X |
| Clean shot | F9 | Y |
| Switch on / off | F6 | L3 + R3 |

While the camera is on the game gets an idle controller: the photo shutter, zoom and every other button are dead, so no scored photo can come from an off-rail angle. F1 is still the options menu; the free camera ignores input while it is open.

**Freeze world** stops the game clock's frame delta (the same stop bytes a pause sets), so Pokemon and effects hold still for a clean shot (checked on Florio: frames 100 and 300 presents apart are pixel-identical). The free camera itself keeps the ride marked as running while the world is frozen. Switching the camera off, or the ride ending, always puts the clock back. Off (the default), the world keeps running: Pokemon keep moving and animating while the NEO-ONE waits. The F1 option *Freeze the world in free camera* (`SNAP_FREECAM_FREEZE=1`) starts every free camera frozen.

**Clean shot** saves `<screenshots>\freecam\freecam_<date>_<time>_<n>.png` (default `%APPDATA%\SnapRecomp\screenshots`). It is a copy of the finished 3D scene taken right after the tonemap and anti-aliasing draws, before the game draws its HUD, at the game's render size (render scale included). The host overlay and F1 menu are never in it.

## Options

| Setting | Key / env | Default |
| --- | --- | --- |
| Free camera | `freecam` / `SNAP_FREECAM=0` turns the hotkeys off | on |
| Freeze the world in free camera | `freecam_freeze` / `SNAP_FREECAM_FREEZE=1` | off |
| Log the camera pose once a second | `SNAP_FREECAM_LOG=1` | off |

## How it works

- **Camera.** The ride camera builder (`main+0x295170`) writes eye, target and up to the camera object (`+0xd0`, `+0xe0`, `+0xf0`), derives the view basis (`main+0x294a50`) and calls `tanf` for the projection (`main+0x296400`, camera object in x20). The `tanf` host override in `lodanim.cpp` calls `snap_freecam_camera` there. It rewrites the three vectors, runs `0x294a50` again so the view data matches, and returns the field of view and a wider far plane. The game's own camera state is never written, so turning the camera off puts the rail camera back exactly. Ultrawide widening, render scale and everything after the builder see the free camera like any other.
- **Rail.** `rt_gm_neo_speed` (`neofilm.cpp`) calls `snap_freecam_rail` after every rail update. While the camera is on the progress is put back to its previous value, so the NEO-ONE stands still and nothing keyed to its position (spawns, scripted events, ride end) fires. The ride clock, Pokemon and effects keep running unless the world is frozen. Nothing else advances differently: when the camera goes off the rail takes its next normal step.
- **Ride end.** `hle::freecam::poll` switches the camera off when no rail has updated for 30 presents (the ride ended, or the game left the ride). The camera hook only runs while a ride camera is built, so a release hook (`set_release_hook`) lets the poll thread unfreeze the clock too.
- **Input.** `Hub::poll` calls `hle::freecam::poll` last. With the camera on it holds the cursor at the window center (mouse look), accumulates movement for the camera hook, and replaces the pad and touch state with a connected, idle pad.
- **Far plane.** The far plane grows by twice the distance flown from the rail so the sky and far scenery stay in view.
- **Bounds.** The camera is not collision tested. Flying under the ground, outside the course or into the sky dome only moves the camera; the game keeps simulating around the paused rail.

## Route script tokens

For tests, route lines (`host/README.md`, "Scripted input") accept: `fc=on|off|toggle`, `fcmove=<right>,<up>,<fwd>` (-1..1 while the line is active), `fclook=<yaw>,<pitch>` (degrees per second), `fcpose=<right>,<up>,<fwd>,<yaw>,<pitch>` (an exact pose, offsets from the rail camera when the free camera went on), `fcfov=<deg>`, `fcshot`, `fcfreeze=on|off`, `fcspeed=<step>`. `fc=on` switches on only while a ride is running. A token acts only while its line is active, but the state it set stays: a route that switches the camera on or freezes the world must say `fc=off` and `fcfreeze=off` itself. Routes: `freecam_florio` (poses, clean shots, round trip), `freecam_score` (free camera and a frozen world mid-ride on Florio, then the whole ride to the Score screen; the log shows the film counter unchanged while the shutter taps keep coming), `freecam_beach` (second course). `SNAP_ROUTE_LOG=1` adds the film counter lines.
