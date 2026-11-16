<!-- SPDX-License-Identifier: MIT -->
# Atmosphere effects

Six display-only effects that work on the linear HDR scene colour before the upscaler: light shafts, height fog, auto exposure, soft glow (bloom), cloud shadows and a sky gradient. All six are off by default. Code: `runtime/nvn/nvn_atmos.cpp`, shaders `runtime/nvn/shaders/atmos_*.frag` (SPIR-V in `runtime/nvn/nvn_atmos_spv.h`, regenerate with `runtime/nvn/shaders/make_atmos_spv.py`). Proof strips: `analysis/atmos/`.

## Switches

| Effect | Env switch | Menu row (tab 16 "Atmosphere") | Settings key |
|---|---|---|---|
| Light shafts | `SNAP_ATMOS_SHAFTS=1` | Light shafts | `atmos_shafts` |
| Height fog | `SNAP_ATMOS_FOG=1` | Height fog | `atmos_fog` |
| Auto exposure | `SNAP_ATMOS_EXPOSURE=1` | Auto exposure | `atmos_exposure` |
| Soft glow | `SNAP_ATMOS_BLOOM=1` | Soft glow | `atmos_bloom` |
| Cloud shadows | `SNAP_ATMOS_CLOUDS=1` | Cloud shadows | `atmos_clouds` |
| Sky gradient | `SNAP_ATMOS_SKY=1` | Sky gradient | `atmos_sky` |

`SNAP_ATMOS=1` turns all six on; an explicit `SNAP_ATMOS_<NAME>=0` still wins. Each row needs a restart. Tab 11 was full (a page holds six rows), so the rows live on a new tab 16 after Lens.

## Display only

The effects are Fx pre-tonemap effects (`nvn_fx.cpp`). That slot never runs on a photo frame, on the Evaluation passes, or inside an occlusion query, so photos and their scores do not see any of it. `Pre()` checks `FxIsPhotoFrame()` again. `SNAP_LIGHT_AB=<P>` switches the effects on in alternate blocks of P presents, which the A/B capture tool (`docs/ab-capture.md`) uses.

## How each one works

- **Light shafts.** A half-resolution pass. For every pixel it marches toward the sun's screen position and counts how much open sky the path crosses, so sky seen through leaves and between trunks becomes soft rays. A contrast power (`SNAP_ATMOS_SHAFT_CONTRAST`) sharpens the rays against the occluders, and an air-mass term (`SNAP_ATMOS_SHAFT_AIR`) keeps the haze thin over nearby leaves and full over distant geometry. The result is weighted by a Henyey-Greenstein phase term and scaled by the scene's mean brightness. The sun is placed relative to the camera (`SNAP_ATMOS_SHAFT_AZIMUTH`, default 25 degrees to the right of the view; `SNAP_ATMOS_SHAFT_ELEVATION`, default 45). It is not read from the game: the game's shadow-caster axis changes with the camera yaw, and its shadow map holds only nearby casters, with no canopy in it.
- **Height fog.** Exponential height fog integrated along each view ray (distance plus height falloff), tinted by the sky colour the frame shows near the horizon (smoothed). The effect caps at `SNAP_ATMOS_FOG_MAX` (0.55).
- **Auto exposure.** A 16x9 tile pass measures a centre-weighted log-average luminance, a 2x1 state image smooths it over `SNAP_ATMOS_EXPOSURE_TIME` seconds (0.7), and the composite applies a gain of `2^((key - measured) * strength)` clamped to 0.6 to 2. The game's HDR scale puts daylight at a mean of about 2.4, so the default key is 1.0 (log2).
- **Soft glow.** A 6-level mip chain: 13-tap downsample with a soft-knee prefilter, tent upsample. The threshold is relative to the adapted mean luminance (`SNAP_ATMOS_BLOOM_THRESHOLD`, 1.5x), so it behaves the same in a dark cave and on a bright beach. At strength 0.2 the glow is visible around the sky and the surf without washing out the picture; 0.4 turned the beach sky cyan in the strip.
- **Cloud shadows.** A four-octave value noise over the world XZ position drifts along a fixed wind direction (`SNAP_ATMOS_CLOUD_SPEED` units per second, noise cell `SNAP_ATMOS_CLOUD_SCALE` units, coverage `SNAP_ATMOS_CLOUD_COVER`). It darkens every non-sky surface by up to `SNAP_ATMOS_CLOUD_STRENGTH` (steep faces get 60% of that; the depth-derived normal is too noisy at world scale to gate on), fading out with distance (`SNAP_ATMOS_CLOUD_FADE`). The game's world units are large: the beach is several hundred units across, so the first guesses (28-unit cells, 700-unit fade) hid the effect completely until the debug view (`SNAP_ATMOS_DEBUG=6`) and a fade sweep showed the fade was zeroing it. The cloud term is a scalar darkening in the composite blend.
- **Sky gradient.** On sky pixels only: a warm glow around the sun's direction, pale haze at the horizon and a deeper zenith (`SNAP_ATMOS_SKY_GLOW`, `_HAZE`, `_ZENITH`, all scaled by `_STRENGTH`). It uses the same camera-relative sun as the shafts, because the game has no world-fixed sun.

All six share one composite: a single full-screen blend (ONE, ONE_MINUS_SRC_ALPHA) on the float scene colour.

## Tunables

Every tunable is a `dev_envf` value: a plain environment variable normally, a live parameter on the F1 Tuning page and in the control channel with `SNAP_DEV=1`.

- Shafts: `SNAP_ATMOS_SHAFT_INTENSITY` (0.9), `_DECAY` (0.95 per step), `_LENGTH` (0.45 of the screen height), `_STEPS` (24), `_G` (0.55), `_AIR` (0.03), `_CONTRAST` (2), `_AZIMUTH` (25), `_ELEVATION` (45); `SNAP_ATMOS_SUN=r,g,b` for the tint.
- Fog: `SNAP_ATMOS_FOG_DENSITY` (0.002), `_FALLOFF` (0.12), `_BASE`, `_MAX` (0.55), `_START` (4).
- Exposure: `SNAP_ATMOS_EXPOSURE_KEY` (1.0), `_STRENGTH` (0.5), `_MIN` (0.6), `_MAX` (2), `_TIME` (0.7).
- Glow: `SNAP_ATMOS_BLOOM_STRENGTH` (0.2), `_THRESHOLD` (1.5), `_KNEE` (0.5).
- Clouds: `SNAP_ATMOS_CLOUD_STRENGTH` (0.3), `_SCALE` (120), `_SPEED` (15), `_COVER` (0.55), `_FADE` (4000).
- Sky: `SNAP_ATMOS_SKY_STRENGTH` (1), `_HAZE` (0.25), `_GLOW` (0.35), `_ZENITH` (0.15).
- Debug views: `SNAP_ATMOS_DEBUG=1` shaft buffer, `8` shaft diagnosis (red: sun behind the camera, blue: open sky along the ray), `3` glow alone, `4` fog amount, `5` exposure gain, `6` cloud cover on the ground and the sky glow. `SNAP_ATMOS_LOG=1` logs the state.

## Cost at 3840x2160

Measured with `host/ab_capture.ps1` (GPU time of the named passes, beach, one effect on at a time, `analysis/atmos/ab_beach_<effect>.json`). The sum includes the shared prefilter and statistics passes, which each effect needs on its own.

| Effect | Passes (ms) | Total (ms) |
|---|---|---|
| Light shafts | shafts 0.24, prefilter and stats 0.14, composite 0.003 | about 0.38 |
| Height fog | prefilter and stats 0.18, composite 0.003 | about 0.18 |
| Auto exposure | prefilter and stats 0.16, composite 0.003 | about 0.16 |
| Soft glow | 6-level chain 0.20, stats 0.06, composite 0.014 | about 0.27 |
| Cloud shadows | prefilter and stats 0.20, composite 0.004 | about 0.20 |
| Sky gradient | prefilter and stats 0.14, composite 0.003 | about 0.15 |

Switching on several at once shares the prefilter and statistics, so the total is lower than the sum of the rows.

## Photos and scores

- By construction: the Fx plumbing never runs these on a photo frame.
- Evidence: see `analysis/atmos/photo_scores.md`.
- Same-photo score proof, tried and not possible (2026-11-16): a checkpoint saved at the start of the Evaluation loaded back with `REFUSED: the address of a released VAMM region is in use` after the score pass, so one process cannot score the same photos twice, and two processes take different photos (the saved photo hashes differ from run to run with identical inputs). The checkpoint run did confirm that all six effects switch on live in a running game.

## Strips and visibility

Off|On strips at 3840x2160 internal render, beach (`core_beach_0`, ride12) and cave (`analysis/atmos/ab_<scene>_<effect>.png`, local only). `mean_abs` is the mean absolute 8-bit difference of the whole frame:

| Effect | Strip | mean_abs | Pixels changed by more than 2 |
|---|---|---|---|
| Light shafts (0.9, contrast 2) | `ab_beach_shafts` | 2.64 | 29.4% |
| Auto exposure | `ab_cave_exposure` | 13.3 | 97.5% |
| Soft glow (0.2) | `ab_beach_bloom` | 10.6 | 73.2% |
| Sky gradient | `ab_beach_sky` | 1.03 | 12.7% |
| Cloud shadows | `ab_beach_clouds` | see the note below | |

The shafts and glow defaults were raised from the first commit (0.35 and 0.1 were hard to see). Glow at 0.4 turned the beach sky cyan (mean_abs 16.6), so the default is 0.2.

## Whole-route bench, the first four on

One exclusive `-Bench` on `florio_photos` with the four effects of the first commit on, build `build\w\atmos`: 16,178 frames, 58.7 fps average, p50 16.7 ms, p90 17.0 ms, p99 23.0 ms, 1% low 16.5 fps (`analysis/perf/bench_florio_photos_20261116_020111.md`). This is one run with no off baseline, taken while other jobs shared the PC, so use it as a sanity check (the game holds its 60 fps cap), not as a cost figure. The per-effect costs above are the numbers to use.
