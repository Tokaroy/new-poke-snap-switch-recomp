# HD shadows

Status (2026-11-15): Stage A (upscale, every course, background build) works and is committed. Stage B (a real GPU ray-traced re-bake) is a working tool that is still being calibrated. Off by default, opt-in, local only.

Goal: sharper, correct sun shadows on every course and time of day, built from the player's own game files in the background. The game's scenery shadows are baked into low-resolution per-mesh textures, so close up they are blocky and soft in the wrong places.

## What the game stores

Every lightmapped mesh object of a course has baked textures in the course pack
(`nx/Pack/LevelComposite/master/LC_<id>.drp/`), named

`<level>^<model>^<hash>^<shape>^<n>^Shadow.nutexb` and `<level>^<model>^<hash>^<shape>^<n>^<k>^Lightmap.nutexb`

- `<model>` is an environment model (`SM_ENV_001_1_Ground`), `<shape>` and `<n>` are the mesh object name and subindex
  inside its `*_LightmapUV.numshb`. Environment models are placed at the identity transform, so their vertices are already
  in world space; every object has a second UV set (`LightMapTextureCoordinate1`) for the baked textures.
- **Shadow** is BC3 (sRGB) with the shadow in alpha and black colour, one mip level. Florio Day leg 1: 328 textures,
  64 to 2048 pixels square. The alpha is almost binary (hard edges, foliage cut-outs), so the game's own look is
  "crisp but blocky".
- **Lightmap** `k = 0` is BC6H bounced light, `k = 1..3` are signed directional terms. The Lighting row of the HD textures
  pack (owned by the hdtex work) upsamples `k = 0`; this feature leaves the light maps alone.
- All courses together: 4,578 Shadow and 9,738 Lightmap textures.
- The only dynamic shadow is one 1024 map around the subject; PCSS in Enhanced lighting only filters that map, so trees,
  rocks and stairs stay as baked.

## What the rival does (NewSnapRecomp 0.9.15, MIT; ideas only, nothing copied)

`newsnap/hdshadows.py` runs Real-ESRGAN over the baked textures: light maps on a log scale with gutter fill, shadow alpha
masks clamped to the original's min/max, soft half-tone regions kept as plain bicubic. It cannot add information.

## Stage A: upscale, read from the game files, every course (done)

Built into the hdtex pipeline (`builder/snapbuild/hdtex*.py`), no model and no play-through:

```
snap_builder.py hdtex --level hd --cats shadow --from-data <unpacked game root> --pack-id hd-shadows \
    --work <work folder> --mods-dir <mods folder> --pause-game
```

- `shadow` is its own texture category. The textures pack never builds it; the HD shadows pack (`hd-shadows`) only builds it.
- Levels: Sharp and HD 2x, Ultra 4x, Custom `shadow.scale=N` (`shadow.strength=0` is plain enlargement, de-ringed).
- `process_shadow`: Lanczos enlargement kept inside the original's local min/max (no ringing, the amount of shadow stays the
  game's), then hard edges, and only those, are steepened by the scale; soft half-tones stay plain. Like the originals, the
  output keeps one mip level.
- `--from-data` scans the unpacked course packs for `^Shadow.nutexb` (4,578 files across 275 packs) instead of the texture
  cache, so nothing has to be played first.
- Background: BelowNormal, `--pause-game` waits while `SnapRecomp.exe` or `snap_boot.exe` runs, progress and estimate
  lines as in the textures build, and every finished texture is recorded, so a stop or a crash resumes where it left off.

Result on Florio Day (crops of the same texture, original at 4x nearest | plain Lanczos | Stage A): Stage A keeps the
game's edges where Lanczos blurs them and is smoother than nearest. `D:\snap-hdshadows\cmp_stageA.png` (local only).

### In-game check (Florio Day, 2026-11-15)

Two frozen-time captures of the same rail position with `host/ab_capture.ps1`, one with an empty mods folder and one with the
HD shadows pack (`host/routes/hds_florio.txt` is `core_florio_a` without the camera sweep, so both runs show the same view).
The pack loads, every ground and tree shadow is where it was, and nothing is missing or shifted. At 4K on this view the two
frames are not visibly different: the ground shadows here are mostly soft and come from the light maps, the Laplacian energy
of the ground band is the same (19.76 against 19.76), so Stage A's gain shows up only on the hard-edged cut-outs, which are
clearest in the texture crops (`cmp_stageA.png`). Stage B has to beat that before it is rolled out.

Harness caveat: `SNAP_MODS_AB` (the same-frame mod swap) drops the ground shadows in the swapped frame even for a same-size
re-encode of the original alpha, so it cannot judge Shadow textures. Use separate frozen-time runs as above. The photo
check stays inconclusive on this route (it takes few shots to keep film).

## Stage B: a real re-bake on the GPU (in progress)

`tools/hd-shadows/` (Rust, `ash`, Vulkan ray query). The course geometry is read with the level editor's readers, one
acceleration structure holds the whole environment (sky and water left out), and each texel of each Shadow texture is
rasterized in the lightmap UV set and traced to the sun through a disc of the sun's angular size (Vogel disk, rotated per
texel). The result is written as BC3 masks at 2x to 4x the original size through `scripts/hd/hdshadows_pack.py`.

Checked so far on Florio Day: 213 of 232 mesh objects match a Shadow file; the lightmap UV set (the last one) with no V
flip reproduces the island layout of the originals exactly; the sun direction is fitted by agreement with the originals
(`calib`), not read from a light entity.

Design rule for the runtime and for later (moving sun, time of day, cloud shadows): the baked sun-shadow factor stays its
own texture, separate from the light map and the ambient term, and the bake takes the sun direction as an argument.

## Sun per course

Fitted by `calib` (azimuth, elevation in degrees, balanced accuracy against the originals). To be filled per course.

| Course | Azimuth | Elevation | Fit |
| --- | --- | --- | --- |
| LC_001_A (Florio Day) | about 5 to 6 | about 45 to 48 | balanced accuracy 0.87 to 0.92 per model |

## Pieces

| Piece | Where |
| --- | --- |
| Stage A (shadow category, `--from-data`, `--pause-game`) | `builder/snapbuild/hdtex*.py`, tests in `builder/tests/test_hdtex.py` |
| Bake tool (Rust, ray query) | `tools/hd-shadows/` (`survey`, `calib`, `bake`), shader `shaders/bake.comp` |
| Originals dump (calibration, sizes) | `scripts/hd/hdshadows_orig.py` |
| Packer (BC3 and mips into a mod pack) | `scripts/hd/hdshadows_pack.py` |
| Launcher row | "HD shadows": Off / On, Build now |

Output only goes under the player's local data folder (`%APPDATA%\SnapRecomp\mods\hd-shadows` for the pack, a work folder
for progress); none of it is committed.
