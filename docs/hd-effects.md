# HD effects

Date: 2026-11-20. Code: `runtime/nvn/nvn_hdfx.cpp` (policy and audit), hooks in `nvn_entry.cpp` (`TextureInitialize`, `ScalePolicy::Follows`),
`nvn_retarget.cpp` (live resize) and `nvn_vk.cpp` (audit at `SetRenderTargets`).

Goal: no part of a 1440p / 4K frame drawn at a fraction of the scene resolution the Switch chose, when that is visible.

## Switches

| Where | Name | Values |
|---|---|---|
| Settings key / `--set` | `gfx_hd_effects` | `0` Off, `1` On |
| Env | `SNAP_HD_EFFECTS` | `0`/`off`, `1`/`on` (2x), or a multiplier 1..4 (only the bloom target goes past 2x) |
| Launcher | Detail page, row "HD effects" | Off / On |
| F1 overlay | Graphics, after Soft particles | Off / On (live) |
| Test aid | `SNAP_HD_EFFECTS_AB=<P>` | odd blocks of P presents forced off (same-view pairs and A/B GPU timing in one run) |
| Test aid | `SNAP_HD_EFFECTS_DEV` (dev param, needs `SNAP_DEV=1`) | -1 follow the setting, 0 off, 2 on; flips live through the control channel |
| Test aid | `SNAP_RT_AUDIT=1` | one `[rtaudit]` line per texture the first time it is bound as a render target |

Default: On with the Enhanced look, Off with the Original look (`hle/visual_defaults.h`, Look row clears `gfx_hd_effects`). Live: the
targets are made again at the next present (`nvn_retarget.cpp`, about 0.2 ms, 18 ms longest frame), except while an upscaler or frame
generation runs (restart then, as for the other size rows).

## Render-target audit (Florio ride, hub, photo select, Evaluation; 3840x2160 window, Enhanced look, render scale 1.5)

Run `1-20261120_213015` with `SNAP_RT_AUDIT=1`. Guest size is what the game asks for; "before" is the image today at scale 1.5
(scene 2880x1620), "after" with HD effects On.

| Target (purpose) | Guest | Before | After | Note |
|---|---|---|---|---|
| Scene colour (RGBA8, RGBA16F, R11G11B10) and scene depth (D32F) | 1920x1080 | 2880x1620 | same | follows render scale |
| Window textures (3, final image and UI) | 1920x1080 | 2880x1620 | same | follow render scale; the present stretches them to the window |
| Sea / splash / dust / pollen layer, colour C (RGBA16F, `main+0x5c09ac`) | 960x540 | 1440x810 | 2880x1620 | half res: water was grainy and soft |
| Downsample B colour + D32F depth (`main+0x5bfde8/0x5bfe08`), the depth the layer tests against | 960x540 | 1440x810 | 2880x1620 | grown with C |
| Bloom (RGBA16F, `main+0x5c23a0`) | 192x108 | 288x162 | 576x324 | |
| Blur / depth-of-field chain, 6 mips (`main+0x5c19ec`) | 960x540 | 1440x810 | same | tried 2x: the blur samples a fixed number of texels, so it blurred less and showed blocks (`crop_lab_dof_on_off.png`) |
| Pair A (`main+0x5be358/0x5be378`) | 960x540 | 1440x810 | same | purpose unknown; not touched |
| Small RGBA8 target | 240x200 | 360x300 | same | follows render scale, not 16:9 |
| Photo (Capture3D JPEG source) | 1080x720 | 1080x720 | same | exempt: scoring and saves |
| Photo thumbnails (BC1 encoder source, 72 per ride) | 360x240 | 360x240 | same | exempt |
| Evaluation depth (D32F_S8) | 1920x1080 | 1920x1080 | same | exempt: photo scoring counts its pixels |
| Dynamic shadow map (D32F, 4 binds) | 1024x1024 | 1024x1024 | same | Shadow resolution row / Enhanced lighting own it |
| Env cubes 256x256 (9 mips, 10), 16x16 cube, 3D LUTs 33^3 and 32^3, 1024x1024x8 3D | - | - | - | loaded textures, never render targets (HD textures work) |

No storage-image (compute-written) targets were made. Host effect modules: the atmosphere glow chain and the lens blur are
deliberately reduced blur pyramids; Enhanced lighting AO uses a 2x2 checkerboard (`SNAP_LIGHT_HALF`, about 1 ms cheaper; not changed here).

## Results

- Same-view pairs (consecutive presents across an A/B flip, run `2-20261120_223219`): the pond and sea lose their half-resolution grain
  and colour noise (`analysis/hd_effects/crop_pond_on_off.png`, first version `crop_water_on_off.png`); foliage and sky unchanged
  (`crop_sky_on_off.png`).
- GPU cost, A/B blocks of 120 presents in one run (shared PC, render scale 1.5): ride median +0.00 ms (block noise about 0.2 ms), hub
  +0.26 ms, all blocks +0.19 ms. The first version that also grew the blur chain and pair A cost +0.5 ms on the ride and looked worse.
- Video memory: about +71 MB at 2880x1620, about +125 MB at 3840x2160 (two RGBA16F and one D32F target at 4x the pixels).
- Photo scoring: see AGENTS.md entry (same photos scored with HD effects on and off).
