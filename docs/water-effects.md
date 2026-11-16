<!-- SPDX-License-Identifier: MIT -->
# Water effects

Four display-only effects on the sea: screen-space reflections, caustics, shoreline foam and FFT ocean waves. All are off by default. Code: `runtime/nvn/nvn_ssr.cpp`, `runtime/nvn/shaders/water_fx.frag` (compiled by `make_water_spv.py` into `nvn_water_spv.h`); the ocean also uses `runtime/nvn/nvn_ocean.cpp` and the module in `runtime/fx/ocean_fft`.

## Switches

| Effect | Env switch | Menu row (tab 17 "Water") | Settings key |
|---|---|---|---|
| Water reflections | `SNAP_WATER_SSR=1` | Water reflections | `water_ssr` |
| Water caustics | `SNAP_WATER_CAUSTICS=1` | Water caustics | `water_caustics` |
| Shoreline foam | `SNAP_WATER_FOAM=1` | Shoreline foam | `water_foam` |
| Ocean waves | `SNAP_WATER_OCEAN=1` | Ocean waves | `water_ocean` |

The rows are live: no restart. Tab 11 had one free slot, so the rows live on a new tab 17 after Atmosphere.

## Display only

The pass never runs on a photo frame, on the Evaluation passes or inside an occlusion query (`FxIsPhotoFrame`, `cx.occOpen`), and it only writes the scene colour. Scores and saved photos do not see it. `host\ab_capture.ps1` checks this for every strip below: no effect draw on a photo present.

## How the game draws the sea

The sea is not in the full-resolution scene pass. The game draws it into a half-resolution target (960x540 colour plus a half-resolution depth, filled from the full depth by a downsample quad), then composites that target into the scene colour with one full-screen quad.

- Four consecutive blended draws make the sea: fragment keys `81169ded7ee5`, `2b28bfe1dbdb`, `6a0d10e3dfbb`, `beae698d6f0b` (`kWaterFs`; `SNAP_WATER_FS=<key,...>` adds more).
- None of them writes depth. The full-resolution depth holds the sea floor and the sand, not the water surface.

Finding them: `NVN_DRAW_LOG` lists the half-resolution pass, and `SNAP_DRAW_SKIP_GROUPS=<keys;keys;...>` skips one key group per 4-present block, so one run shows which draws make the sea.

## How the pass works

`WaterBeforeDraw` is one hook line in `DoDraw` (after `FxMidPass`).

1. First water draw: end the render pass, copy the half-resolution depth (`hdPre`), force depth writes on for the water draws (`WaterForceDepthWrite`, applied to the pipeline in `DoDraw`).
2. First draw after the water: copy the depth again (`hdPost`: it now holds the water surface wherever the sea is visible) and copy `hdPre` back, so the rest of the frame sees the game's own depth.
3. The composite draw (first draw into the scene colour after that): copy the scene colour first, so reflections sample the finished image.
4. The next draw: one full-screen pass over the scene colour, blended `dst = src.rgb + dst.rgb * src.a`. It never reads its own target.

Water pixel = `hdPost` differs from `hdPre`. The floor is the full-resolution depth; a pixel is dropped when something opaque is nearer than the water.

- **Reflections.** The reflected ray is marched through the full depth (steps that grow with distance, then a 5-step binary refine), so what is on screen reflects. A ripple term bends the normal. Fresnel weights it, and the weight fades to zero at the screen edge. A miss leaves the game's own cubemap reflection untouched (alpha 1, rgb 0).
- **Caustics.** A bright net on what lies under the water, from the depth of water above the floor: strongest in shallows, gone below about 6 units and with distance.
- **Foam.** A lace band where the water is thin (water surface height above the floor), pulsing with time and broken up with noise.
- **Ocean waves.** `runtime/fx/ocean_fft` (a Phillips-spectrum FFT ocean, three 128x128 cascades with 256, 1024 and 4096 unit tiles) runs one compute update per frame from `RunPass` (`nvn_ocean.cpp`, outside any render pass, time = presents / 60). The pass reads its normal and foam maps at the world position of each water pixel. The normal replaces the small built-in ripple term in the reflection (so it needs Water reflections for the swell to show), and the persistent crest foam is drawn as whitecaps with the same foam colour as the shoreline foam. The game's sea mesh is not displaced: the waves are in the shading only, so the shoreline, the depth test and everything the game reads back stay as they were. The module needs `shaderStorageImageExtendedFormats` (the device enables every core feature the GPU has); if it is missing or the create fails, the row turns itself off for the run and the log says why.

## Tunables

Plain environment variables; live parameters on the F1 Tuning page with `SNAP_DEV=1`.

- `SNAP_WATER_SSR_STRENGTH` (1), `SNAP_WATER_SSR_DIST` (45 world units), `SNAP_WATER_RIPPLE` (0.02), `SNAP_WATER_CAUSTICS_STRENGTH` (1), `SNAP_WATER_FOAM_STRENGTH` (1, also scales the whitecaps), `SNAP_WATER_OCEAN_STRENGTH` (1, slope multiplier of the ocean normal).
- Debug views: `SNAP_WATER_DEBUG=1` water mask, `2` water height above the floor, `3` mask terms, `4` view distance of the three depths, `5` world height of the depths. `SNAP_WATER_LOG=1` probe (distinct scene fragment keys, pass stats), `SNAP_WATER_LOG=2` one line per frame with the reason the hook did nothing, `SNAP_WATER_TIME=1` GPU ms of the copies and the pass.
- With `SNAP_DEV=1` the four switches are dev parameters too, so `ab_capture.ps1 -Effect SNAP_WATER_SSR -Values 0,1 -Set SNAP_WATER_SSR=1` flips only the water effect.

## Proof

Same animation moment, off then on, `host\ab_capture.ps1 -Route core_beach_0 -Moment beach_sea`:

- `analysis/water/ab_beach_ssr.png`
- `analysis/water/ab_beach_caustics.png`
- `analysis/water/ab_beach_foam.png`

Each strip has a JSON next to it with the pixel diff, the photo-frame check and the per-pass GPU times.

## Cost at 3840x2160

GPU time of the full-screen pass at 3840x2160 on the beach (`host\ab_capture.ps1` per-pass deltas, `SNAP_WATER_TIME=1`), measured while other jobs ran on the same GPU, so read them as upper bounds. An exclusive `-Bench` is still to do.

| Effect | Extra GPU time |
|---|---|
| Reflections | about 0.5 ms (pass) plus about 0.15 ms for the depth copies and the forced depth writes on the sea draws |
| Caustics | about 0.08 ms |
| Shoreline foam | about 0.07 ms |

The copies and the pass run only while a water effect is on and the sea is on screen.

Scores: the three strips pass the photo-frame check (no effect draw on any photo present). A full Florio ride to the Score screen with all three effects on reached the Score screen normally (run `0-20261116_101539`). A same-photo on/off score comparison was not possible: the checkpoint load after the Evaluation is refused (`a VAMM region changed size`), and two separate rides shoot different photos.

## Limits

- Needs the sea's fragment keys. Courses that draw water with other programs (river, lake, reef) are not covered until their keys are added to `kWaterFs`.
- The half-resolution depth is a downsample, so a reflected edge can step by one half-resolution texel.
- The depth format must be D32_SFLOAT; otherwise the effect turns itself off for that frame.
- A short stretch at the start of a ride (about 25 presents on the beach) has no valid course camera, and the pass skips it.
