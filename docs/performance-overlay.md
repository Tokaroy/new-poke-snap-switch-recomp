# Frame time overlay (F3)

Date: 2026-11-15. Press F3 in the game to show how the game is running. Code: `runtime/ui/ui_fps.cpp`, drawn by the host overlay (`ui_overlay.cpp`).

F3 cycles Off, FPS, Detailed, Off. The choice is saved. If the player bound F3 to a game button in the controls map, F3 stays the game's and the overlay is set from F1 > Graphics > Frame rate instead.

| Mode | Shows |
|---|---|
| FPS | One number, the average over the last 0.5 s (green at 55+, yellow at 28+, red below) |
| Detailed | The same number, frame time in ms, the 1% low (the average rate of the slowest 1% of frames in the last 10 s), CPU ms and GPU ms, the render size and output size, the upscaler and anti-aliasing line (for example `DLSS ... SMAA CAS 0.30`), the frame cap and vsync, and a graph of the last 2.5 s of frame times with a blue line at the target frame time |

CPU ms is the presenting thread's CPU time per frame. GPU ms is the busy time of the GPU timestamp pairs around every command buffer, collected one frame late. Both are switched on only in Detailed mode; Off costs one atomic load per frame.

The corner is a setting (`fps_overlay_corner`: `tl`, `tr`, `bl`, `br`; default top right). It scales with the output height, so it stays readable at 4K. The overlay is drawn after the game's frame is composed, so it never appears in photos, the album or the game's own captures.

Settings: key `fps_overlay` (`fps`, `detailed`, or unset), environment variable `SNAP_FPS_OVERLAY`, corner `SNAP_FPS_OVERLAY_CORNER`. Full notes: `host/README.md`, "Performance overlay".

## Not the same as F4

F4 is the developer GPU-cost overlay (`SNAP_DEV=1` only): milliseconds per named render pass. F3 is the one for players and bug reports. Neither needs the other.
