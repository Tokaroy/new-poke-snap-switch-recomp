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

Other courses, in game (2026-11-16): the same pair of runs for Jungle and Sands loaded the all-course pack without errors, but
a matched Off and On frame was not obtained. The route's end-of-ride moment drifts with machine load (one capture landed on the
pause menu, one on a different scene), so those frames cannot be compared pixel for pixel. Only the Florio pair above is a
matched in-game view.

Every course, from the files (2026-11-16, `scripts/hd/hdshadows_verify.py` on a seeded sample of 12 maps per pack, 326 maps over all 29 packs):
each output shrunk back to the original size matches the original alpha, mean absolute difference 0.0032 overall (worst pack
0.0093), at least 99.2% of pixels on the same side of 50% in every map, and the half-tone share stays close to the original
(for example 4.9% to 5.1% on LC_001_A). Every sampled map is exactly 2x the original size. No size mismatches.

Harness caveat: `SNAP_MODS_AB` (the same-frame mod swap) drops the ground shadows in the swapped frame even for a same-size
re-encode of the original alpha, so it cannot judge Shadow textures. Use separate frozen-time runs as above. The photo
check stays inconclusive on this route (it takes few shots to keep film).

## Stage B: a real re-bake on the GPU (every course, 2026-11-21)

`tools/hd-shadows/` (Rust, `ash`, Vulkan ray query; `build.rs` compiles the shaders with glslc). The course geometry is read
with the level editor's readers, every level of a course pack is loaded (each time-of-day level has its own sun), and two
acceleration structures hold the environment (sky and water left out): one-sided geometry and cut-out cards in one, culled per
ray by the level's flags, and two-sided geometry in an instance with culling off. Cut-out materials (alpha-tested foliage)
come from each material's `baseColorTexture` alpha (`scripts/hd/hdshadows_alpha.py` writes the masks; BC1, BC3 and BC7) and
are traced as non-opaque geometry, confirmed in the ray query where alpha is over 102/255. Each texel of each Shadow texture
is rasterized in the lightmap UV set (the last one, no V flip) and traced to the sun through a disc of the real sun's size
(0.27 degrees, 32 samples, Vogel disk rotated per texel). File names that carry an instance number (`GrassX3` for `GrassX31`)
are matched by prefix, so every Shadow file of Florio Day (328 of 328) finds its mesh.

**Calibration** (`hd-shadows calib`, per level): a 4 degree grid over the sky on about 150,000 thinned texels, for 4 culling
flag sets and both polarities, in batched dispatches (many sun directions per dispatch, about 100 s per course), then the best
4 refined at 1 and 0.25 degrees, then the ray bias and the penumbra radius. Only texels the game shows lit by the sun are
scored: covered, facing the sun (the game applies N.L at runtime, so back faces are free), and on the solid part of a card.
Flags 3 (no N.L in the bake, back faces culled) fit best almost everywhere; fitting two-sided models one by one gave no gain on
Florio and is opt-in (`--fit-two-sided`). Result: `tools/hd-shadows/sun.tsv`, the table below.

**Bake and fallback** (`hd-shadows bake`, then `scripts/hd/hdshadows_pack.py`): each map is baked at 2x (HD) or 4x (Ultra)
the original size, at most 4096, then scored shrunk back to the original size against the original on the same texels as
`calib` (`report.tsv`). The packer uses the re-bake only where it scores balanced accuracy 0.85 or more and agreement 0.88 or
more (fewer than 32 scored texels: agreement 0.9); every other map is the Stage A file. Inside a re-baked map, only the scored
texels (sun-facing, solid; `<stem>.care.png` from the bake) take the re-bake: back faces, gutters and see-through card parts
keep the Stage A value (the game's own map, enlarged, when there is no Stage A file), so nothing the score did not judge
changes. The 15 non-square maps (LC_005_1, LC_009) keep Stage A.

Result, all 29 packs at HD (2026-11-21, RTX 4090, BelowNormal): 4,578 maps; 2,585 re-baked, 1,981 Stage A, 12 left as the
game has them (no Stage A file and no passing re-bake). Bake 54 minutes (a second 21-minute pass added the scored-texel masks; a fresh run makes both in one pass), pack
5.8 GB (Stage A 3.9 GB).
`hdshadows_verify.py` on every map of the pack: mean absolute difference 0.0085 against the originals (Stage A 0.0032), worst
map agreement 0.922 (Florio Day 0.952). What falls back: the trees, flowers and grasses whose original shadows are painted or
blocky per island (Florio `Flower2`/`Flower3` near 0.6, the seasonal trees of LC_006 0 of 95, `GrassX` 20 of 114). What
re-bakes: rock, cliff, ground, cave, wall, crystal and building geometry (`Cave`, `WallrockA`, `CrystalA` 100%).

### In-game check (2026-11-21)

Frozen-time 4K captures (`host/ab_capture.ps1`, 3840 x 2160, render scale 2, hidden and muted, `SNAP_MODS_DIR` set to a
folder with only the pack under test): Florio Day with Stage A and with the re-bake (`hds_florio`, ride 40), and the re-bake
alone on Blushing Beach (`core_beach_0`, ride 3), Outaway Cave (`core_cave_0`, ride 10) and Florio Night (`live_florio_night`,
ride 40). The pack loads (4,566 overrides), every ground, rock and tree shadow is where it was, and nothing is missing, shifted
or speckled. At 4K the Florio Day pair is not visibly different, as with Stage A against the original: most of what the
player sees is soft light-map shading, and the maps that would change the most (foliage) are the ones that fall back. In the
textures the re-bake keeps the originals' shapes, loses their per-texel stair steps on rock and ground, and adds thin casters
the originals lost (vines and branch tips in the cave). Scoring: `florio_eval` with only the re-baked pack reached
`ui=evaluation:Score` and the end of the route (exit 0). Frames and the sheet stay local:
`analysis/hdshadows/hdshadows_stageB_contact.png`. Harness note: `ab_capture.ps1`'s compare step needs PIL, which
`tools/venv-recomp` does not have; `host/ab_compare.py` runs with a Python that has it.

Design rule for the runtime and for later (moving sun, time of day, cloud shadows): the baked sun-shadow factor stays its
own texture, separate from the light map and the ambient term, and the bake takes the sun direction as an argument.

## Sun per course

Fitted by `calib` per level (azimuth and elevation in degrees, culling flags, balanced accuracy of the fit against the
originals on sun-facing texels). Mean 0.902; the lowest are LC_005_2_A 0.820, LC_001_B leg 1 0.834, LC_007_1_B 0.835 and
LC_005_2_B 0.838, below Florio Day leg 1 (0.841): their maps that miss the 0.85 bar per map use Stage A.

| Course pack | Level | Azimuth | Elevation | Flags | Fit |
| --- | --- | --- | --- | --- | --- |
| LC_001_A | LV_ENV_001_1_A | 5.25 | 47.00 | 3 | 0.841 |
| LC_001_A | LV_ENV_001_2_A | 237.50 | 79.25 | 3 | 0.902 |
| LC_001_B | LV_ENV_001_1_B | 107.75 | 59.75 | 3 | 0.834 |
| LC_001_B | LV_ENV_001_2_B | 11.75 | 24.50 | 3 | 0.952 |
| LC_001_C | LV_ENV_001_1_C | 227.25 | 29.75 | 3 | 0.879 |
| LC_003_1_A | LV_ENV_003_1_A | 352.25 | 44.25 | 3 | 0.849 |
| LC_003_1_B | LV_ENV_003_1_B | 313.75 | 39.75 | 3 | 0.853 |
| LC_003_2_A | LV_ENV_003_2_A | 43.50 | 69.75 | 3 | 0.882 |
| LC_003_2_B | LV_ENV_003_2_B | 350.50 | 7.25 | 3 | 0.942 |
| LC_004_1_A | LV_ENV_004_1_A | 138.00 | 62.50 | 5 | 0.889 |
| LC_004_3_C | LV_ENV_004_3_C | 198.25 | 69.25 | 3 | 0.941 |
| LC_005_1_A | LV_ENV_005_1_A | 17.75 | 69.50 | 3 | 0.853 |
| LC_005_1_B | LV_ENV_005_1_B | 290.00 | 54.25 | 3 | 0.877 |
| LC_005_2_A | LV_ENV_005_2_A | 174.50 | 72.25 | 3 | 0.820 |
| LC_005_2_B | LV_ENV_005_2_B | 193.50 | 66.75 | 3 | 0.838 |
| LC_006_3_C | LV_ENV_006_3_C | 211.75 | 70.00 | 3 | 0.976 |
| LC_006_A | LV_ENV_006_1_A | 308.75 | 69.50 | 3 | 0.961 |
| LC_006_A | LV_ENV_006_2_A | 309.00 | 69.75 | 0 | 0.864 |
| LC_007_1_A | LV_ENV_007_1_A | 104.50 | 23.50 | 0 | 0.903 |
| LC_007_1_B | LV_ENV_007_1_B | 225.50 | 25.75 | 5 | 0.835 |
| LC_007_2_A | LV_ENV_007_2_A | 178.25 | 47.00 | 0 | 0.916 |
| LC_007_2_B | LV_ENV_007_2_B | 108.00 | 59.75 | 0 | 0.906 |
| LC_008_3_C | LV_ENV_008_3_C | 197.50 | 69.50 | 3 | 0.925 |
| LC_008_A | LV_ENV_008_1_A | 277.50 | 43.00 | 3 | 0.951 |
| LC_009_A | LV_ENV_009_1_A | 133.50 | 46.75 | 3 | 0.854 |
| LC_009_B | LV_ENV_009_1_B | 252.75 | 45.75 | 3 | 0.891 |
| LC_010_1_A | LV_ENV_010_1_A | 228.50 | 75.50 | 3 | 0.971 |
| LC_010_3_C | LV_ENV_010_3_C | 103.50 | 1.25 | 0 | 0.983 |
| LC_011_3_C | LV_ENV_011_3_C | 155.25 | 85.00 | 3 | 0.950 |
| LC_011_A | LV_ENV_011_1_A | 145.75 | 41.50 | 3 | 0.958 |
| LC_101_A | LV_ENV_101_1_A | 145.25 | 50.50 | 3 | 0.878 |
| LC_103_A | LV_ENV_103_1_A | 331.00 | 62.00 | 5 | 0.999 |

## Pieces

| Piece | Where |
| --- | --- |
| Stage A (shadow category, `--from-data`, `--pause-game`) | `builder/snapbuild/hdtex*.py`, tests in `builder/tests/test_hdtex.py` |
| Bake tool (Rust, ray query) | `tools/hd-shadows/` (`survey`, `calib`, `bake`), shader `shaders/bake.comp` |
| Originals dump (calibration, sizes) | `scripts/hd/hdshadows_orig.py` |
| Packer (BC3 and mips into a mod pack) | `scripts/hd/hdshadows_pack.py` |
| Pack check (output shrunk to original size vs original alpha) | `scripts/hd/hdshadows_verify.py` |
| Re-bake in the builder (`hdtex --shadow-source rebaked`) | `builder/snapbuild/hdshadows_rebake.py` (pack `hd-shadows-rebaked`), cut-out masks `scripts/hd/hdshadows_alpha.py` |
| Launcher row | "HD shadows": Off / Upscaled / Re-baked, then the size (Sharp, HD, Ultra) |

Output only goes under the player's local data folder (`%APPDATA%\SnapRecomp\mods\hd-shadows` and `hd-shadows-rebaked`, only one of them on at a time; a work folder
for progress); none of it is committed.
