<!-- SPDX-License-Identifier: MIT -->
# Atmosphere effects

Four display-only effects that work on the linear HDR scene colour before the upscaler: light shafts, height fog, auto exposure and soft glow (bloom). All four are off by default. Code: `runtime/nvn/nvn_atmos.cpp`, shaders `runtime/nvn/shaders/atmos_*.frag` (SPIR-V in `runtime/nvn/nvn_atmos_spv.h`, regenerate with `runtime/nvn/shaders/make_atmos_spv.py`). Proof strips: `analysis/atmos/`.

## Switches

| Effect | Env switch | Menu row (tab 16 "Atmosphere") | Settings key |
|---|---|---|---|
| Light shafts | `SNAP_ATMOS_SHAFTS=1` | Light shafts | `atmos_shafts` |
| Height fog | `SNAP_ATMOS_FOG=1` | Height fog | `atmos_fog` |
| Auto exposure | `SNAP_ATMOS_EXPOSURE=1` | Auto exposure | `atmos_exposure` |
| Soft glow | `SNAP_ATMOS_BLOOM=1` | Soft glow | `atmos_bloom` |

`SNAP_ATMOS=1` turns all four on; an explicit `SNAP_ATMOS_<NAME>=0` still wins. Each row needs a restart. Tab 11 was full (a page holds six rows), so the rows live on a new tab 16 after Lens.

## Display only

The effects are Fx pre-tonemap effects (`nvn_fx.cpp`). That slot never runs on a photo frame, on the Evaluation passes, or inside an occlusion query, so photos and their scores do not see any of it. `Pre()` checks `FxIsPhotoFrame()` again. `SNAP_LIGHT_AB=<P>` switches the effects on in alternate blocks of P presents, which the A/B capture tool (`docs/ab-capture.md`) uses.

## How each one works

- **Light shafts.** A half-resolution pass. For every pixel it marches toward the sun's screen position and counts how much open sky the path crosses, so sky seen through leaves and between trunks becomes soft rays. The result is weighted by a Henyey-Greenstein phase term and scaled by the scene's mean brightness. The sun is placed relative to the camera (`SNAP_ATMOS_SHAFT_AZIMUTH`, default 25 degrees to the right of the view; `SNAP_ATMOS_SHAFT_ELEVATION`, default 45). It is not read from the game: the game's shadow-caster axis changes with the camera yaw, and its shadow map holds only nearby casters, with no canopy in it.
- **Height fog.** Exponential height fog integrated along each view ray (distance plus height falloff), tinted by the sky colour the frame shows near the horizon (smoothed). The effect caps at `SNAP_ATMOS_FOG_MAX` (0.55).
- **Auto exposure.** A 16x9 tile pass measures a centre-weighted log-average luminance, a 2x1 state image smooths it over `SNAP_ATMOS_EXPOSURE_TIME` seconds (0.7), and the composite applies a gain of `2^((key - measured) * strength)` clamped to 0.6 to 2. The game's HDR scale puts daylight at a mean of about 2.4, so the default key is 1.0 (log2).
- **Soft glow.** A 6-level mip chain: 13-tap downsample with a soft-knee prefilter, tent upsample. The threshold is relative to the adapted mean luminance (`SNAP_ATMOS_BLOOM_THRESHOLD`, 3x), so it behaves the same in a dark cave and on a bright beach, and the strength is low (0.1) so nothing washes out.

All four share one composite: a single full-screen blend (ONE, ONE_MINUS_SRC_ALPHA) on the float scene colour.

## Tunables

Every tunable is a `dev_envf` value: a plain environment variable normally, a live parameter on the F1 Tuning page and in the control channel with `SNAP_DEV=1`.

- Shafts: `SNAP_ATMOS_SHAFT_INTENSITY` (0.35), `_DECAY` (0.95 per step), `_LENGTH` (0.45 of the screen height), `_STEPS` (24), `_G` (0.55), `_AZIMUTH`, `_ELEVATION`; `SNAP_ATMOS_SUN=r,g,b` for the tint.
- Fog: `SNAP_ATMOS_FOG_DENSITY` (0.002), `_FALLOFF` (0.12), `_BASE`, `_MAX` (0.55), `_START` (4).
- Exposure: `SNAP_ATMOS_EXPOSURE_KEY` (1.0), `_STRENGTH` (0.5), `_MIN` (0.6), `_MAX` (2), `_TIME` (0.7).
- Glow: `SNAP_ATMOS_BLOOM_STRENGTH` (0.1), `_THRESHOLD` (3), `_KNEE` (0.5).
- Debug views: `SNAP_ATMOS_DEBUG=1` shaft buffer, `8` shaft diagnosis (red: sun behind the camera, blue: open sky along the ray), `3` glow alone, `4` fog amount, `5` exposure gain. `SNAP_ATMOS_LOG=1` logs the state.

## Cost at 3840x2160

Measured with `host/ab_capture.ps1` (GPU time of the named passes, beach, one effect on at a time, `analysis/atmos/ab_beach_<effect>.json`). The sum includes the shared prefilter and statistics passes, which each effect needs on its own.

| Effect | Passes (ms) | Total (ms) |
|---|---|---|
| Light shafts | shafts 0.27, prefilter and stats 0.20, composite 0.03 | about 0.50 |
| Height fog | prefilter and stats 0.18, composite 0.003 | about 0.18 |
| Auto exposure | prefilter and stats 0.16, composite 0.003 | about 0.16 |
| Soft glow | 6-level chain 0.18, stats 0.08, composite 0.014 | about 0.27 |

Switching on several at once shares the prefilter and statistics, so the total is lower than the sum of the rows.

## Photos and scores

- By construction: the Fx plumbing never runs these on a photo frame.
- Evidence: see `analysis/atmos/photo_scores.md`.

## Whole-route bench, all four on

One exclusive `-Bench` on `florio_photos` with `SNAP_ATMOS=1` (all four effects), build `build\w\atmos`: 16,178 frames, 58.7 fps average, p50 16.7 ms, p90 17.0 ms, p99 23.0 ms, 1% low 16.5 fps (`analysis/perf/bench_florio_photos_20261116_020111.md`). This is one run with no off baseline, taken while other jobs shared the PC, so use it as a sanity check (the game holds its 60 fps cap), not as a cost figure. The per-effect costs above are the numbers to use.
