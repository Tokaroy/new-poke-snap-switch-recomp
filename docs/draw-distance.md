# Distant scenery (draw distance and far detail)

Far-away scenery and Pokemon keep their detailed models farther out, and models are drawn farther out before they pop away. Display only: what spawns, what Pokemon do and how photos are scored stay the game's own.

## Setting

| Where | Values | Default |
|---|---|---|
| `SNAP_DISTANT_SCENERY` | `off`, `high`, `ultra` | `off` |
| `--distant-scenery off\|high\|ultra` | same, this start only (`original` and `0` mean off) | off |
| F1 > Graphics > View: "Distant scenery" (`gfx_distant_scenery`) | Original, High, Ultra | Original |

| Step | Detail distance (LOD switches) | Draw distance (where models stop being drawn) |
|---|---|---|
| Original | 1x, the game's own | 1x |
| High | 3x | 2x |
| Ultra | 8x | 4x |

It applies at once, with no restart. The two older rows below it, "Detail distance" (`gfx_lod_dist`, `SNAP_LOD_DIST`) and "Draw distance" (`gfx_draw_dist`, `SNAP_DRAW_DIST`), still work and can push either distance beyond the step: the larger multiplier of the two wins. "Always full detail" and "Full-rate distant animation" are not part of the steps (the second one changes how often far Pokemon refresh their pose).

## What the game decides, and what this touches

The game's model class (vtable `main+0xc7c5e8`, update `main+0x334db0`) picks a level of detail per model every frame at `main+0x337ca8` (main model) and `main+0x337ab8` (attached sub-models): the share of the screen the bounding sphere covers (`ratio`, falls with distance squared) is compared with a 3-float table per LOD group. At or above the first value it is LOD0, then LOD1, LOD2, and below the last value LOD3, which clears the model's visible byte (`+0x104`) so its render job is skipped. Because the ratio falls with distance squared, multiplying it by m squared moves every threshold out by m. `host/mods/lodanim.cpp` `lod_apply` does that in the existing hooks, with one multiplier for the LOD0/1/2 switches and another for the LOD3 cull. No new hook, no guest code patched.

| Decision | Where | Render only? |
|---|---|---|
| LOD0/1/2 switches | the model's LOD fields (`+0x110`, `+0x118` of the render object), from the ratio hook | Yes. They choose which mesh set is drawn. |
| LOD3 cull (not drawn) | the same hook, clears the visible byte | Yes. In the whole executable the only byte reads of that field at that offset in game code are in the update function itself (it skips the model's render submission) and one unrelated material descriptor at `main+0x5f437c`. |
| Camera far plane | not found; the cut-off seen in courses is this cull | Not changed. |
| Terrain, sky, water | not model instances | Not changed. |
| Distant animation rate (counter `+0x4170`) | `rt_gm_anim_rate` | Left out of the steps. |
| Photo scoring inputs | the Evaluation re-render at photo select | Held at the game's own picks, see below. |

Spawning and Pokemon behavior run in their own objects; the scan found no reader of the model's visible byte there (limits under "Scoring").

## Scoring

Photo scores are computed from the Evaluation re-render of each photo (GPU scoring draws, the body-part sphere function `0x426520`, the object and visibility lists; see `photo-scoring.md`). That re-render happens at photo select, one photo per present, and the LOD picks it makes must be the game's. `snap_wide_eval_copy` runs once per photo Evaluation, and from there until 10 presents after the last one `in_photo_eval()` is true and `lod_apply` leaves every pick alone. Nothing about the scoring path is hooked or changed.

Check, 2026-11-23 (`SNAP_TRACE_LODSEL=1 NVN_EVAL_LOG=1 NVN_EVAL_LOG_MAX=40000`, Ultra, `florio_eval`, run `0-20261123_071012`): the ride changed 1,388,314 LOD picks, then photo select re-rendered all 72 photos in presents 14190 to 14261 with 10,365 logged scoring draws (Aggregate, Sphere, CopyDepth). The trace line per present (`lodanim: present N: K picks kept the game's (photo Evaluation), M changed by the mod`) exists for 80 presents (14188 to 16200, which also covers the Professor and Re-snap screens); every scoring-draw present is among them, and the sum of M over all of them is 0. The route reaches `ui=evaluation:End` with Off, High and Ultra.

Not proven: a same-photo score comparison (the route's photos differ from run to run and a save state cannot be loaded at photo select), and the claim that no game code reads the visible byte through a pointer the static scan cannot follow.

## Cost

Florio ride, `bench_florio` (timed in seconds, 2560x1440, uncapped, exclusive slots, other workers' jobs may still have run): average over the ride window (18,000 to 20,000 frames each):

| Step | Runs | Average fps | Median frame |
|---|---|---|---|
| Original | 2 | 121.8 and 136.7 | 7.50 and 7.00 ms |
| High | 1 | 127.0 | 7.46 ms |
| Ultra | 2 | 126.3 and 128.0 | 7.43 and 7.35 ms |

The spread between the two Original runs is larger than the difference between steps, so the cost is within noise on this PC (RTX 4090 class GPU, GPU frame about 2.3 ms either way in the frozen A/B frames). A weaker CPU or GPU may see more: Ultra draws far more models and builds their render jobs.

## Proof frames

`host\ab_capture.ps1 -Route dd_florio -Moment florio_start -Effect opts -Values 'gfx_distant_scenery=off|gfx_distant_scenery=high|gfx_distant_scenery=ultra'` rides to the same frozen frame and switches the row. Florio (rail 8): the far tree line behind the Dodrio goes from sparse leaf cards to dense foliage; Off against High changes 15.6% of the pixels. Freeze check passes. Frames stay under `analysis\drawdist\` (not committed, game images).

## Files

`host/mods/lodanim.cpp` (`lodmods_init`, `lod_apply`, `tally`), `runtime/ui/ui_options.cpp` (row), `runtime/hle/settings.cpp` (key to env, live list), `host/cli_flags.h` (`--distant-scenery`), `tests/hle/cli_flags_test.cpp`, `docs/command-line.md`, `docs/option-map.md`. Not in any quality preset.
