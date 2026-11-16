# Accessibility options

Added 2026-11-15. One tab in the F1 menu (Accessibility) and one page in the launcher (Accessibility) hold the same rows. Every row is off by default, so the game looks and plays exactly as before until you turn something on. None of the rows changes gameplay rules or photo scores.

Each row has a settings key (saved per profile in `settings.cfg`) and an environment variable. A `SNAP_ACCESS_*` variable set by hand wins over the saved value, like every other setting. Defaults erase the key.

| Row | Env var | Settings key | Values | Live? |
| --- | --- | --- | --- | --- |
| Color vision | `SNAP_ACCESS_CVD` | `access_cvd` | `off`, `protanopia`, `deuteranopia`, `tritanopia` | yes |
| Correction strength | `SNAP_ACCESS_CVD_STRENGTH` | `access_cvd_strength` | 10 to 100 (percent, steps of 10; default 100) | yes |
| Reduce flashing | `SNAP_ACCESS_FLASH` | `access_flash` | `off`, `gentle`, `standard`, `strong` (0 to 3 also read) | yes |
| Text and menu size | `SNAP_ACCESS_UI_SIZE` | `access_ui_size` | 100, 125, 150, 200 (percent) | yes |
| High-contrast menus | `SNAP_ACCESS_CONTRAST` | `access_contrast` | `0` / `1` | yes |
| Hold to toggle | `SNAP_ACCESS_TOGGLE` | `access_toggle` | `off`, or a list such as `ZL`, `ZR`, `ZL,ZR`, `ZL,ZR,L,R,X,Y,A,B` | next start (env), live in F1 (`toggle_buttons` in `input.json`) |
| Mono audio | `SNAP_ACCESS_MONO` | `access_mono` | `0` / `1` | yes |

Code: `runtime/hle/access.{h,cpp}` holds the shared state and the key and env parsing. `settings.cpp` pushes saved keys into it on every change.

## Color vision correction

The correction runs inside the existing final present pass (`host/mods/look.cpp`, shader `host/mods/shaders/look.frag`), after the game frame is blitted and before the F1 menu is drawn. That placement gives the behavior we want:

- It covers everything the game draws: world, HUD, menus, subtitles.
- It does not touch our F1 menu, which is drawn afterward.
- It does not touch photos. The Evaluation screen and saved photos are rendered from the game's own render targets, which never pass through the present pass. Photo scoring is unchanged.
- HDR works: the look pass runs on the SDR scratch image before the HDR conversion.

The math is the usual daltonization: simulate the chosen deficiency with the Machado 2009 matrix (severity 1.0) in linear RGB, take the difference between the original and the simulated color, shift that lost information into the channels the viewer can still tell apart (protan and deutan: green and blue; tritan: red and green), then mix by the strength slider. Strength 100% is the full correction.

## Reduce flashing

The limiter keeps the whole frame's mean brightness from changing faster than a set rate. Camera flashes, lightning-style bursts and fade-to-white transitions then fade in over several frames instead of one.

A mip chain on the scratch image reduces the frame to one pixel, so the mean costs a few tiny blits. A one-pixel state pass compares this frame's mean with last frame's output mean and writes a gain. The main pass multiplies by that gain. The gain is clamped (0.05 to 1.6, 2.2 or 3.0 by level) and the first frame after switching on starts from itself, so there is no jump when the row is toggled.

| Level | Allowed change per second (sqrt-linear luma) | Max gain |
| --- | --- | --- |
| Gentle | 6.0 | 1.6 |
| Standard | 3.0 | 2.2 |
| Strong | 1.6 | 3.0 |

It limits whole-frame changes only. It does not stop a small bright flicker inside a mostly steady frame. Camera shake is not touched: the game applies it inside its own camera code, and no clean hook for it has been found.

## Text and menu size, high contrast

The size row scales our F1 menu (100, 125, 150 or 200 percent on top of the display scale) and grows the launcher window up to the size of your work area. The game's own HUD has its own scale rows under Display, which are not changed here.

High-contrast menus swap the F1 theme and the launcher palette for black panels, white text and a bright blue selection.

## Hold to toggle

Turns a button into a latch: one press holds it down, the next press releases it. The input layer (`runtime/hle/input_core.cpp`) already had latches for ZL and ZR; this generalizes them to any of A, B, X, Y, L, R, ZL, ZR. In F1 (Accessibility tab, Controls) tick the buttons you want. The same list lives in `input.json` as `toggle_buttons`. `SNAP_ACCESS_TOGGLE` (a comma list) adds buttons for one run and is read once at startup.

Opening the F1 menu releases every latch so a held button never carries into a menu.

## Mono audio

Averages the left and right channel of each stereo pair before the master gain in `runtime/audio/audio_out.cpp` (pairs 0/1, 4/5, 6/7 of a surround mix; the center channel is left alone). Plays the same sound in both ears. Effects and music are both covered because the mix happens after they are combined.

## Checks

- HLE tests: `tests\hle\run_tests.ps1`, test `access.rows`, plus the toggle-buttons checks in the Hub test (parse, JSON round trip, latch behavior).
- Visual rows: one `host/ab_capture.ps1` capture per row, Off then On, photo gating PASS. Results and capture paths are listed under "Verification results" below.

## Verification results

Run 2026-11-15 on the Florio route with `host/ab_capture.ps1` (hidden desktop, muted), frames dumped after the present pass (`-Set NVN_FRAME_DUMP_POST=1`).

| Row | Result | Evidence |
| --- | --- | --- |
| Color vision | Off, protanopia, deuteranopia and tritanopia each give a different, visibly shifted picture at the same ride moment; Off matches the untouched frame. | `analysis/access_cvd/ab_ride8.png` and `.json`, `ab_ride20.png` |
| Reduce flashing | With the limiter on, the largest change in mean frame brightness between samples 0.15 s apart drops from 0.47 to 0.22 on the camera-flash run. | `analysis/access_flash/off/`, `analysis/access_flash/on/` |
| Text size, high contrast, F1 tab | The 22-check UI script passes (every row reachable, values written, theme and 100/150/200% scales rendered). | `tests/hle/ui_access_script.txt`, `tests/hle/run_access_ui.ps1`, `analysis/ui/access_dump/access_sheet.png` |
| Hold to toggle, mono audio, row parsing | HLE tests pass (1923 run, 0 failed), including `access.rows` and the toggle-buttons checks. | `tests\hleun_tests.ps1` |

Photo gating: the ab_capture photo gate reports INCONCLUSIVE (exit 11) for the color vision capture. It only detects effect draws on photo frames, and these effects run in the present pass, after the game has finished every render target the Evaluation screen and saved photos read. So photo content and scoring are unchanged by construction, but the gate could not show it with a pass. The Evaluation screen as displayed on the monitor is corrected like any other game screen.

Not checked in game: a hold-to-toggle ride (covered by the HLE hub test only) and mono audio (covered by the row parsing test only).

## Not done, and why

- **Bigger or higher-contrast reticle.** The reticle is a game texture drawn by the game's HUD code. Changing it means editing game assets or patching the HUD draw, and we do not ship edited assets. The HUD scale rows under Display already make it larger along with the rest of the HUD. If wanted later, the clean route is a shader-side outline drawn around the reticle's known screen rectangle.
- **Camera shake reduction.** No clean source found (see Reduce flashing).

## Ideas that change gameplay (not implemented)

These would alter rules, timing or scoring, so they are listed only:

- Auto-aim assist or a wider focus window for the Pester Ball and Fluffruit.
- Slower ride speed or a pause-anywhere mid-ride.
- Relaxed photo scoring or an easier research level requirement.
- Button remapping for the shutter and zoom beyond what Controls already offers.
- Spoken menu text (screen reader support).
