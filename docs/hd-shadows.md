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
snap_builder.py hdtex --level hd --cats shadow --from-romfs <extracted romfs folder> --pack-id hd-shadows --pause-game [--json]
```

- `shadow` is its own texture category. The textures pack never builds it; the HD shadows pack (`hd-shadows`) only builds it.
- Levels: Sharp and HD 2x, Ultra 4x, Custom `shadow.scale=N` (`shadow.strength=0` is plain enlargement, de-ringed). Shadow maps
  stop at 2048 pixels, so an original that is already 2048 stays as the game has it.
- `process_shadow`: Lanczos enlargement kept inside the original's local min/max (no ringing, the amount of shadow stays the
  game's), then hard edges, and only those, are steepened by the scale; soft half-tones stay plain. Like the originals, the
  output keeps one mip level.
- `--from-romfs` reads the course archives directly: `scripts/drp_unpack.py --name '\^Shadow\.nutexb$'` decodes the 4,578
  shadow maps of the 29 course packs (1.3 GB, 64 s) into the work folder, so nothing has to be played first and no unpacked
  game tree is needed. `--from-data` takes an already unpacked tree instead.
- Background: BelowNormal, `--pause-game` waits while `SnapRecomp.exe` or `snap_boot.exe` runs, and every finished texture is
  recorded, so a stop or a crash resumes where it left off ("N of M already up to date").
- `--json` prints `{"progress": {...}}` lines and a final `{"done": {...}}` for the launcher's progress bar; `--dry-run` prints
  `{"plan": {...}}`. Each pack keeps its own work folder, so this build never touches the HD textures state.

On the reference PC (4,493 of 4,578 maps are planned; 16 are skipped by the classifier and 69 are 2048 already): one course
(203 maps) built at about 340 maps a minute, so the estimate for all courses was about 13 minutes of build time and 3.8 GB on
disk at HD. Measured for all of them (2026-11-16, CPU only, three runs because of `--limit` chunks and a resume): 4,493 maps built,
0 failed, 3.9 GB on disk, 12.7 minutes (3.7 + 5.9 + 3.1).

| | NewSnapRecomp 0.9.15 | This build |
| --- | --- | --- |
| Every course at install, no play-through | yes | yes (`--from-romfs`) |
| Painted shadows on stairs, decks, rocks, ground | yes | yes, every per-mesh sun-shadow map of every course |
| Edges cleaned, soft foliage left soft | yes (ESRGAN, clamped) | yes (clamped Lanczos, hard edges only steepened) |
| Pauses when the game starts, resumes | yes | yes |
| Time | about 35 min, RTX 4060 | 12.7 min measured, CPU only (4090 PC) |
| Disk | 6.3 GB | 3.9 GB at HD |
| Progress bar | 0.9.20 | JSON progress events; launcher row pending |

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
