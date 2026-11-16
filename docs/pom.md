# Parallax detail (POM)

Gives ground, rock and bark depth: the surface shifts with the viewing angle and its crevices shade themselves. Display only: the depth buffer, silhouettes, collision and photo scores do not change (it writes color only, no depth, no discard, and photo frames are skipped).

- Setting: `gfx_pom` / `SNAP_POM` = `off` (default) | `low` | `high`. F1 > Graphics "Parallax detail" (the launcher shows the same row). Restart is not needed: classification happens for every texture.
- Needs a local table: `python -I scripts\hd\make_pom_table.py` writes `build\pom\pom_table.txt` (ground, rock and bark textures of your own dump, keyed like the shiny mod: FNV-1a 64 of the first 4096 bytes). Also read from `%APPDATA%\SnapRecomp\mods\pom\pom_table.txt` or `SNAP_POM_TABLE=<file>`. Never committed.
- Tuning (env): `SNAP_POM_DEPTH` (fraction of a texture tile, default 0.015), `SNAP_POM_FADE=<start>,<end>` world units (default 14,40), `SNAP_POM_SHADOW` (0..1, default 0.6), `SNAP_POM_FLIP=1`, `SNAP_POM_LOG=1`.

## How it works (runtime/nvn/nvn_pom.cpp)
- A draw whose fragment stage samples a table texture gets a fragment-program variant (pad2 bits 8..16 of the pipeline key). The patch runs after the game stores its interpolated UV and replaces the UV with a steep-parallax result (8 steps Low, 16 High, linear interpolation at the hit), then darkens the first base-color fetch by a short march toward the sun (3 / 6 steps).
- There is no height map: height is the base color's luminance against its own mip-5 average, so no extra texture or binding.
- The view ray needs no vertex tangents: world position comes from FragCoord, depth and the camera's inverse view-projection (the contact-shadow math); UV gradients come from screen derivatives.
- Push constants: fragment offset 128..255 (RT shadows use 0..127, so the two never combine; RT-shadow draws keep their own path). Every pipeline layout carries a 256-byte fragment range when the device allows it.
- A no-op effect is registered so the Fx layer tracks the scene color and camera while the level is above Off.

## Measured (Florio, `core_florio_a`, ride8, frozen frame, `host\ab_capture.ps1 -Effect SNAP_POM`, other runs sharing the GPU)
GPU ms per frame, mean of two passes per value, default depth:

| Scene size | Off | Low | High |
|---|---|---|---|
| 3840x2160 | 2.02 | 2.30 (+0.28) | 2.41 (+0.39) |
| 2560x1440 | 1.22 | 1.95 (+0.7, noisy) | 1.59 (+0.4, noisy) |
| 1920x1080 | 1.25 | 1.70 (+0.45) | 1.77 (+0.52) |

The 1440p row varies by more than the effect (one High pass 1.34, the other 1.84); read it as "about +0.4 to +0.7 ms". Low and High cost nearly the same, so the cost is the shader's fixed work, not the step count.

## Known limits
- Self-shadow uses the dynamic shadow caster's axis as the sun (fixed fallback high sun at reduced strength when no caster is seen); it is an approximation.
- The default depth (0.015) was picked from one capture at 0.01 (0.05 smeared the ground); the fade distances are unmeasured guesses. High shows some stair-stepping on grass blades.
- No distance early-out: far pixels of a table texture still pay the full march (the fade only scales the offset).
- Not yet run: HLE/CRC tests, a ride to Score on this build, validation-layer pass over the patched modules (patched variants created without errors in the capture runs).
