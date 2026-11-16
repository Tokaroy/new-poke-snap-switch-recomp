# Enhanced lighting (2026-11-15)

One preset, `SNAP_LIGHTING=enhanced` (menu: Settings, Graphics, "Lighting preset"; launcher: Lighting), replaces the three separate lighting
switches. Default stays Original: Enhanced costs frame time at 4K (numbers below), so it is opt-in until the shadow cost is cut.
Every piece has its own env switch and falls back to the original path.

## Pieces

| Piece | Where | What it does |
| --- | --- | --- |
| PCSS soft shadows | `runtime/nvn/nvn_shadowpcf.cpp` | The receiver fragment SPIR-V is rewritten: a blocker search (8 taps) finds the average occluder depth, the penumbra width follows blocker distance, then 16 Vogel-disk PCF taps. Shadows are sharp at the contact point and soften with distance. Valid (`spirv-val`) on all 856 receiver shaders at 4, 8, 16 and 32 taps with and without PCSS (`tests/nvn/pcf_test.sh`). Applies to the moving shadow map and to static maps. |
| Shadow map from render scale | `runtime/hle/lighting_preset.h` | Smallest power of two at or above 1080 x render scale (2048 to 8192); Maximum is one step larger. Shadow distance 2x (Maximum 4x). |
| GTAO | `runtime/nvn/nvn_lighting_fx.cpp`, `runtime/nvn/shaders/lfx_ao.frag` | Ground-truth AO (Jimenez et al. 2016 technique, own implementation): 8 steps, 2 slices (Maximum 3), per-pixel noise shifted every frame, normals from depth. |
| Denoise | `runtime/nvn/shaders/lfx_resolve.frag` | Depth-aware 5x5 bilateral blur, then temporal accumulation with reprojection (previous matrix x inverse current), disocclusion rejection and neighborhood clamp. |
| Contact shadows | same pass, `runtime/nvn/shaders/lfx_ao.frag` | The `runtime/nvn/shaders/contact.frag` march with the artifacts fixed: per-pixel dithered start, slope-scaled bias, fade at screen borders and with distance, and the same denoise as AO. |
| Composite | `runtime/nvn/shaders/lfx_comp.frag` | Multiplies the linear HDR scene colour (pre-tonemap slot) by `ao * contact`. |

The AO and contact passes run in the Fx pre-tonemap slot at the first blended draw, so sky and particles are not darkened, and they are skipped on
photo frames (`FxIsPhotoFrame`), so the photo itself and its scoring never see them. The shadow changes (map size, distance, PCSS) do apply to photo frames; they change shadow pixels only.

Nothing was adapted from NewSnapRecomp (MIT); no notice needed. GTAO follows the published paper.

## Switches

`SNAP_LIGHTING=enhanced|maximum` (`high`, `ultra` are aliases), `SNAP_SHADOW_PCSS=0|1` (+ `_LIGHT`, `_MAX`, `_SEARCH`, `_FLIP`), `SNAP_LIGHT_FX=0|1`,
`SNAP_LIGHT_AO=0`, `SNAP_LIGHT_CONTACT=0`, `SNAP_LIGHT_AO_RADIUS`, `_AO_STRENGTH`, `_AO_POWER`, `SNAP_LIGHT_CONTACT_STRENGTH`, `_CONTACT_LEN`, `_CONTACT_THICK`,
`SNAP_LIGHT_FADE`, `SNAP_LIGHT_ALPHA`, `SNAP_LIGHT_DEBUG` (1 AO, 2 contact, 3 raw AO), `SNAP_LIGHT_AB=<P>` (effects on in odd presents: same-view A/B).
Details and defaults: `host/README.md` lighting table.

## Proof

Same camera, original vs Enhanced (4K, render scale 2), `analysis/lighting/compare/` (git-ignored): `flA_*` Florio day, `flB_*` Florio night, `cave_*` cave;
`*_sbs.png` is a wipe, `*_orig.png` / `*_enh.png` the full frames. Mean luma of Enhanced / original: Florio day 0.83 to 0.94, night 0.88 to 0.94, cave 0.92 to 0.96.
AO grounds grass and tree bases and darkens crevices; PCSS shows as shadows that sharpen toward the contact point. Vulkan validation: 0 errors on Florio day, night and cave runs.
Regenerate with `scripts/light/run_light_compare.ps1` and `scripts/light/pair_light.py`.

## Photo scoring

Three kinds of evidence, none a clean same-photo A/B:

- The scoring route (`score_time_a`, game-second taps) reaches Score and End with Enhanced and with Original, same subject (Dodrio).
- The photo each run takes differs run to run even without lighting changes (two Original runs scored 2,337 and 2,445; a third 2,658; Enhanced 2,432), so totals cannot isolate the preset on this route.
- By construction the AO and contact passes skip photo frames, and scoring reads actor and depth state, not shadow pixels.

A byte-level same-frame check (one photo shot rendered with and without the preset) is still open.

## Cost (4K window, render scale 1, `SNAP_FPS=144`, cap 0, exclusive bench, ride from present 3000)

| Run | avg fps | median ms | p99 ms | 1% low fps | new pipelines |
| --- | --- | --- | --- | --- | --- |
| Original | 131.0 / 121.5 | 6.98 / 7.06 | 18.8 / 20.9 | 40 / 35 | 2 |
| Enhanced | 107.0 / 96.1 | 7.12 / 7.23 | 31.4 / 37.1 | 17 / 5 | 105 |
| Maximum | 118.3 | 7.05 | 24.1 | 20 | 105 |
| Enhanced shadows only (`SNAP_LIGHT_FX=0`) | 114.6 | 7.34 | 27.8 | 22 | 99 |
| AO and contact only (`SNAP_LIGHT_FX=1`) | 124.7 | 7.07 | 22.3 | 29 | 2 |

Original run to run varies by about 10 fps on this PC, so read the table by size: the AO and contact passes cost within that noise; the shadow side (PCSS receivers,
4096 map, 2x distance) carries the cost, mostly in the frame-time tail, not the median. Enhanced does not hold 144 at 4K, so it is not the default.
Maximum landing above Enhanced is noise, not a speed-up.

Next: cut the shadow cost (cascade distance back toward 1.5x, PCSS search taps 4, pre-warm the 100 patched receiver pipelines at load).
