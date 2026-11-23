# Smooth camera

Above the game's frame rate (a 120, 144 or 240 Hz screen with the game at its own 60 fps), turning and panning the camera moves on
every refresh instead of in 60 Hz steps. Each extra frame shows the newest game frame from a camera between the last two game
cameras (camera render interpolation). The game itself still runs at its own rate: timing, Pokémon, scoring and your photos are
unchanged.

| Setting | Values | Default |
| --- | --- | --- |
| `SNAP_SMOOTH_CAMERA` | `1` on, unset or `0` off | off |
| Settings key `smooth_camera` | `1` / `default` | `default` (off) |
| Command line | `--smooth-camera on\|off` (this start only), or `--set smooth_camera=1` | |
| Menus | F1 and native Settings: Graphics / Frame rate "Smooth camera"; launcher Display page under Frame rate target; native PC tab Upscaler | |

Applies after a restart (the swapchain and the scene tracking are set up at start).

## What it does

- The camera of every game frame (the PerViewCBuffer the scene draws read, `docs/motion-vectors.md`) and its scene depth are
  kept at the scene's upscale point; right after the game's tonemap and anti-aliasing draw, the finished 3D picture is copied
  before the game draws its HUD and menus (the same HUD-less copy frame generation uses).
- Each game frame is then presented as 1 to 8 pictures on the display clock, one display period apart and one game frame late.
  A picture shown at time t shows the camera of time t minus one game frame, between the last two game cameras: rotation is
  slerped, position and projection (zoom) are interpolated. The newest game picture is re-projected to that camera with its
  depth (a few fixed-point steps per pixel, `runtime/nvn/shaders/smoothcam_warp.comp`); pixels where the finished frame
  differs from the HUD-less copy (HUD, menus, dialogue, fades) are copied from the finished frame unchanged.
- On a 144 Hz screen with the game at 60 fps that is 2 or 3 pictures per game frame (2.4 on average), each with a new camera.

## Limits

- Only the camera is smoothed. Pokémon, particles, water and the HUD still move once per game frame, so against a smoothly
  turning background a moving Pokémon keeps its 60 Hz steps.
- Adds about one game frame of display delay (16.7 ms at 60 fps); the game reads input as before.
- Re-projection is from one game picture: when the camera moves sideways, edges of near objects can show a thin smear where
  the newer picture has nothing behind them, and the screen edge repeats its last pixels while turning. Fast turns of more
  than 30 degrees in one game frame and jumps of more than 5 units are treated as cuts (no in-between pictures).
- Off with frame generation (the two do not stack) and with HDR output (the HDR present path does not take a substitute
  picture yet). Photo frames (the shutter's photo pass, the upscaler's photo guard) are shown as they are.
- Gate: on while the present rate is at least 1.5 times the game rate (off again below 1.35 times). The present rate is the
  monitor's refresh, lowered by the Frame rate limit row. With Frame rate target above 60 the game itself runs faster and the gate
  usually stays shut.

## Photos and scoring

Nothing the game reads is written. The re-projected pictures exist only in the module's own images and the swapchain; the
game's command lists, render targets, photo targets, the Evaluation passes and the saved JPEGs are the same as with it off. The
game frame count (`g_presentCount`, which routes and the shutter hold count) still goes up once per game frame.

## Measured (2026-11-23)

Florio rides (`florio_eval`) in a hidden 4K window with `SNAP_SMOOTH_CAMERA_HZ=144`, on a PC shared with other jobs:

- Run `0-20261123_073833`: reached `ui=evaluation:End`; in the ride 144 presents a second at a 60 fps game rate, 127 to 144 of
  them re-projected; no frame without a scene copy, 201 camera cuts.
- Present to present while the camera turns (`scripts/smoothcam_trace.py`): 1.6% of presents show no camera change and the
  step coefficient of variation is 1.59. The same 144 Hz schedule without re-projection (run `0-20261123_072437`): 63.3% and
  2.99, the 60 Hz stair step.
- Frame dumps with `SNAP_SMOOTH_CAMERA_TEST_ALPHA=0.5` (run `0-20261123_080832`): the re-projected picture is the scene moved
  part of the way, with the reticle, film counter and buttons where the real frame has them.
- Scores: a ride with it off (`0-20261123_074457`) and one with it on (`0-20261123_075056`) both scored their photos (Vivillon
  1,544 and 1,876). The rides took different photos, so this is not a same-photo comparison.

## Test aids

| Variable | Effect |
| --- | --- |
| `SNAP_SMOOTH_CAMERA_HZ=<hz>` | present rate instead of the monitor refresh and the frame rate limit (hidden test desktops) |
| `SNAP_SMOOTH_CAMERA_PAUSE=1` | gate shut: plain presents, still traced (the "off" side of a comparison) |
| `SNAP_SMOOTH_CAMERA_LOG=1` | one stderr line a second: game fps, presented fps, re-projected fps, and counters (frames prepared, without a scene copy, cuts) |
| `SNAP_SMOOTH_CAMERA_TEST_ALPHA=<0..1>` | each game frame is one re-projected picture at that camera position, then the frame itself, whatever the frame time; for frame dumps (`NVN_FRAME_DUMP_GEN=1`), which stall the game |
| `SNAP_SMOOTH_CAMERA_TRACE=<csv>` | one row per present: time, game frame, pass, alpha, re-projected, shown camera yaw / pitch / position |

`python -I scripts/smoothcam_trace.py <trace.csv> --hz 144` models a fixed-refresh panel and reports how even the camera steps
between refreshes are while turning (zero steps = repeated frames, coefficient of variation of the steps).

Code: `runtime/nvn/nvn_smoothcam.{h,cpp}` (images, passes, present plan), `runtime/nvn/smoothcam_core.h` (in-between camera
and schedule, tested by `tests/hle/smoothcam_tests.h`), hooks in `upscale/upscale.cpp` (Resolve), `nvn_draw.cpp` (after the
tonemap draw) and `nvn_vk.cpp` (`PresentWindowOnce`); the presents go through the presenter thread of `nvn_framegen.cpp`.
