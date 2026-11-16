# Graphics presets

One row, **Graphics preset**, sets the picture rows together: Original, Balanced, Enhanced, Max, or Custom. It is the first row of the launcher's Picture page, the first row of the F1 menu's Graphics tab, and the first row of the Quality tab in the game's own Settings screen.

Code: `runtime/hle/presets.h` (one table for all three places), checked by `tests/hle/picture_rows_test.cpp`.

## How the row works

- A preset is a set of ordinary stored settings (the same keys the rows below it use). Nothing new is saved and nothing new reaches the game.
- The row itself is not saved. It is worked out from the other rows every time it is drawn. Change any single row and the preset row shows **Custom**. Put the rows back to a preset's values and it shows that preset again.
- **Custom** can be shown but never picked.
- A fresh install has no saved picture rows, so it reads **Enhanced**.
- Picking a preset clears the rows it does not set, then writes the ones it does. Restart-only rows follow the usual "needs a restart" count in the F1 menu.

## What each preset sets

| Row | Original | Balanced | Enhanced | Max |
|---|---|---|---|---|
| Look | Original | Enhanced | Enhanced | Enhanced |
| Anti-aliasing and upscaler | the game's own FXAA | best installed upscaler at Quality, else SMAA | Look default (supersampling sized to the screen + SMAA) | DLAA if DLSS is installed, else 2x supersampling + SMAA |
| Internal resolution | Automatic | Automatic | Automatic | Automatic |
| SMAA quality | High | Medium | High | Ultra |
| Anisotropic filtering | Original look: off | 8x | 16x (Look default) | 16x |
| Sharpening, menu text, PNG photo files, HD photos | Original look: off | Look default | Look default | Look default |
| Lighting preset | Original | Original | Original | Maximum |
| Frame rate target | 60 (default) | 60 | 60 | 120 |
| Leaf edge smoothing | off | off | off | on |
| Leaf translucency | off | off | off | Subtle (0.5) |
| Soft particles | off | off | off | on |
| Zoom bokeh | off | off | off | Light (0.6) |
| Ambient occlusion | Default | Default | Default | Ultra (256 samples; shows on the photo-select screen) |
| Light shafts, height fog, auto exposure, soft glow | off | off | off | on |
| Water reflections, water caustics, shoreline foam | off | off | off | on |
| Bounce light | off | off | off | on |

"Look default" means the row is erased, so the Look decides it. Enhanced is the new-install state: every row at its default under the Enhanced look.

The upscaler cell depends on the PC. Balanced and Max pick it from the files installed next to the exe (DLSS, FSR, XeSS). A preset written before an upscaler was installed still reads as that preset afterwards.

## Left out on purpose

These are not part of any preset, so picking one never moves them:

- Draw distance, always-full-detail and full-rate animation: photo scoring is unverified for them.
- Detail distance: display-only (photo scoring keeps the game's models), but it costs speed, so it stays a separate choice.
- Frame generation and motion blur: a matter of taste or hardware, not of quality level.
- HDR, color, HUD scale, the frame rate limit, vsync and the display mode: set per screen, not per look.
- Shadow resolution, shadow distance, texture sharpness and the render-pass switches: the older **Detail preset** row (F1 and the launcher) owns these.

## Joins Max when committed

Only rows that are committed are mapped. These have rows in the working tree but are not committed yet, so no preset touches them. Each is added to the Max column in the commit that lands it:

- Parallax detail (`gfx_pom`)

Balanced and Enhanced turn none of the effect rows on: Enhanced is what a new install runs, and Balanced is meant to cost less than it. Only Max turns the effects on. The cost of that split is measured below (Balanced is lighter than Enhanced); move a row into Balanced when its own cost is known to be small.

To add one: put a row in the `rows()` table in `runtime/hle/presets.h` (the value for Max, `""` for the others), and the test checks it.

## What each preset costs

Measured 2026-11-16 on the Florio Nature Park ride (`host/routes/bench_florio.txt`, about 3.6 minutes of riding), 2560x1440 window (`SNAP_WINDOW_W/H`), the NVIDIA upscaler installed. Each preset ran twice: a warm-up run that fills the shader cache, then the measured run, alone on the PC (the benchmark takes every run slot). The build was the working tree of that day, so it holds other people's uncommitted work too. One measured run per preset.

| Preset | Avg fps | 1% low fps | GPU busy per frame (mean) | GPU busy share of the ride |
|---|---|---|---|---|
| Original | 60.0 | 50.1 | 2.28 ms | 14% |
| Balanced | 59.4 | 26.0 | 3.25 ms | 19% |
| Enhanced | 58.1 | 13.6 | 3.79 ms | 22% |
| Max | 100.7 (target 120) | 13.5 | 3.31 ms | 33% |

How to read it:

- **Balanced is lighter than Enhanced**, so it keeps its place: 0.5 ms less GPU time per frame (3.25 against 3.79 ms), 3 points less GPU busy share. DLSS at Quality draws about 960 lines where Enhanced draws 2160 lines and scales them down. No change to the table is needed.
- The 60 fps limit hides most of the difference: Original, Balanced and Enhanced all sit at about 60 fps, so GPU time is the number that tells them apart.
- **Max** draws at the window's own 1440 lines (DLAA) instead of Enhanced's 2160, which pays for part of the effects: its GPU time per frame is about Enhanced's, at 100 fps instead of 60, so the GPU is busy a third of the time. The gap to 120 fps is not GPU time; the cause (CPU work or frame pacing) was not measured.
- The 1% low figures are dominated by hitches (pipeline builds, streaming) and by whatever else the PC was doing, so do not rank presets by them. Max has three frames of 16 to 19 seconds around present 10408 (left out of the 100.7 fps and the 1% low; with them Max reads 80.4 fps and a 1% low of 3.1), during the minutes the owner was using the PC. Treat the Max row as a first estimate and rerun it on an idle PC.
- GPU busy is the per-frame `gpu_busy_ms` of the SNAP_PERF log, summed over the ride window between the route's `ride` and `ride_end` waits; avg fps and 1% low come from `dt_ms` over the same frames.

## Older rows

The F1 menu and the launcher keep **Detail preset** (Very Low to Maximum; key `gfx_preset`), called Quality preset until the Graphics preset row arrived. It sets render scale, shadows, occlusion, sharpening and filtering by weight class and leaves an upscaler alone. It is gone from the game's own Settings screen, where its slot went to Graphics preset (six rows fit on the tab).
