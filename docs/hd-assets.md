# HD assets: preview pipeline and viewer

Goal: preview higher-resolution textures, and later better models, outside the game, with before/after comparison. Then pack the good ones as a mod that the game loads through the RomFS overlay.

Everything produced from game data stays local, under `build/hd/`, which is git-ignored. The repo holds only code.

## Phase 1: textures and preview (HD work paused 2026-11-14, see "Paused" at the end)

There are two parts, joined by the cache layout below.

1. **Pipeline:** `scripts/hd/`, Python, run with `-I` or `-E -P`.
   - It finds an asset's model and textures in `extracted/drp-v262144/`.
   - It converts the SSBH model (numdlb/numshb/numatb/nusktb) to glTF 2.0, mesh and skeleton only, with no textures embedded.
   - It decodes the textures to PNG.
   - It runs local AI upscalers on them.
2. **Viewer:** `tools-src/asset_viewer/`, a static web app (three.js) plus a tiny localhost server. It reads the cache and shows the assets before and after.

### Cache layout (the contract)

```
build/hd/cache/
  manifest.json
  <asset_id>/
    model.gltf (+ .bin)          mesh, skeleton, UVs, material slots; textures referenced by name only
    textures/<tex>.orig.png      decoded original (RGBA8, all mips dropped)
    textures/<tex>.<variant>.png upscaled variants, e.g. x4_realesrgan, x4_anime
    info.json                    per-asset details (LODs, material -> texture map)
```

`manifest.json`:

```json
{
  "version": 1,
  "variants": [{"id": "x4_realesrgan", "label": "Real-ESRGAN x4plus", "scale": 4}],
  "assets": [{
    "id": "pm0025_00",
    "name": "Pikachu",
    "category": "pokemon|course|character|prop|ui",
    "model": "pm0025_00/model.gltf",
    "lods": ["pm0025_00/model.gltf", "pm0025_00/model_lod1.gltf"],
    "materials": [{"name": "body", "albedo": "tex_a", "normal": "tex_n", "mask": "tex_m", "emissive": null}],
    "textures": [{
      "name": "tex_a",
      "role": "albedo|normal|mask|emissive|other",
      "orig": "pm0025_00/textures/tex_a.orig.png",
      "w": 512, "h": 512, "format": "BC7",
      "variants": {"x4_realesrgan": "pm0025_00/textures/tex_a.x4_realesrgan.png"}
    }]
  }]
}
```

### Viewer features

- An asset list with search and category filter.
- **3D view:** orbit and zoom, a variant toggle (original or any upscale), a split-screen A/B mode, wireframe, LOD switch, and a neutral studio light plus an HDRI-free sky.
- **Texture view:** a before/after slider at 1:1 and zoomed, pixel-peeping with nearest filtering, and per-channel display (RGB, A).

## Phase 2: in-game test (after phase 1 looks good)

1. Re-encode the chosen upscales to nutexb, in the same format as the originals (BC7/BC1/BC5, mips, block-linear).
2. Place them in a mod folder that the HLE `rom` overlay mounts over RomFS.
3. Compare screenshots in game.

## Phase 3: better models (later)

- A numshb/numdlb writer.
- Retargeting the existing skeletons and rigs onto higher-detail meshes.
- Sources can be the owner's own game data, models we make ourselves (subdivision and sculpting, or local generation), or third-party model files the owner chooses for personal use.
- Third-party files live only in `build/hd/external/` with a local `SOURCES.md` provenance list, and are never committed or distributed. Only model and texture formats are allowed, never executables.

## AI re-texture (SD1.5 + tile ControlNet, 2026-11-12)

Local only. Models in `tools/sd/` (see `tools/sd/SOURCES.md`), venv `tools/venv-sd`. Script: `scripts/hd/retexture_sd.py` (run with `tools\venv-sd\Scripts\python.exe -I`), contact sheets: `scripts/hd/sd_sheet.py` -> `analysis/hd/sd_<tex>.png`.

- Method: wrap-pad (if the source tiles), Lanczos 4x, 512 px tiles with 128 px feathered overlap, img2img strength 0.4 plus tile ControlNet, per-channel histogram match back to the source, alpha resampled only. Output `<tex>.x4_sd.png`, variant `x4_sd` in the manifest. `--max-seconds N` pauses and resumes (the 2048 ground = 8192 px takes about 7.5 min on a shared 4090; other textures 10 s to 2 min).
- Verified: mean RGB within 0.6 of the source on groundB, groundC and bark; edge wrap difference no worse than the source on those three.
- Verdict from contact sheets: the flower, bark and groundB variants are visibly different at a glance and faithful. It adds legible structure (pebbles, bark plates, petal veins), but it is smoother and more painterly than the original grain. At strength 0.4 and above SD invents content: rockK (terraced cracks, dark holes) and rockD (moss turned to straw) drift, so rocks are not ready. Lower strength (0.25-0.3) trades detail for faithfulness.
- Not run on normal or mask maps.

### groundB round 2 (owner: round-1 SD "a bit smooth")

Sheet: `analysis/hd/groundB_round2.png` (1:1 crop and 4x zoom per column; test crop = 256 px source centre). Seam check: `analysis/hd/groundB_scan_seams.png`.
- A (SD retuned: sharper prompt/negatives, cn 1.3, 40 steps, DPM++ 2M Karras, strength 0.4, unsharp) and B (ESRGAN low + SD high, gain 1.0/1.5): still soft; SD at 512 px tiles cannot invent sharp grain from a blurry 4x input. Not worth shipping over round 1.
- C (`scripts/hd/detail_scan.py`, variant `x4_scan`): ESRGAN x4 base + high-pass (sigma 3, gain 0.5) of an ambientCG CC0 dirt scan (Ground111, `build/hd/external/`), histogram-matched to the source. Crisp, photo-like grain, layout and colors unchanged; mean RGB within 0.43 of the source (full 8192 px). Caveat: fine speckle also lands on smooth stones.
- Tileability: the source groundB itself is not seamless (edge wrap difference 8.8 vs neighbor 2.1); x4_scan adds only about 1.3 to it (the 1024 px scan tile divides 8192).
- D (SDXL + tile ControlNet) not installed: A to C did not need it.

### x4_blend (owner pick, 2026-11-12)

The owner rejected x4_scan (grain) on groundB and prefers Real-ESRGAN and SD, or a blend. `scripts/hd/blend_variants.py` writes `<tex>.x4_blend.png` = 50/50 RGB blend of x4_realesrgan and x4_sd (alpha from Real-ESRGAN), then an additive per-channel mean lock to the source (mean diff at most 0.25 on all). Made for flower, grassB, bark, groundB (8192), groundC, groundD, rockF; not rockD or rockK (SD drift). Every variant is at most 8192 px. Real-ESRGAN dif variants were created for these textures with the existing ncnn tool (`-n realesrgan-x4plus`). Seen on a groundC crop: the SD half adds a hatched pattern and shifts the leaf layout, and the blend keeps a faint trace of it.

## Phase 2 result (2026-11-12)

- **Tools:** `scripts/hd/detail_boost.py` makes `<tex>.x2_detail.png` for tiling environment textures: Lanczos 2x, plus the texture's own finest band-pass detail re-used one octave finer (tiled with a shift, zero mean, so colors do not move), a mild unsharp, and a bump term from that band added to the normal map. Mask and emissive maps get a plain 2x. `scripts/hd/make_hd_pack.py` writes a pack from the cache in the original nutexb format (BC1/BC3/BC5 as the source, full mip chain, block-linear, capped at 4096). Choice order for albedo: x4_blend (not groundC), x4_sd (bark, flower), x2_detail. Emissive (BC6H) is not encoded.
- **In game:** pack `build/mods_hd_florio/hd-florio` (28 Florio textures) loaded with 28 `[mods] hit` lines and no crash. Size 2x and 4x of the original works. Crops in `analysis/mods/compare/hd_ground_*` and `hd_path_*`: finer grain, crisper pebbles and grass, same colors. The gain is modest, and the ground is still magnified at 1080p.
- **Magenta wash:** not caused by larger textures. With only `hud-00024/25` (960x540) replaced at 1x, the same full-screen wash shows at the same fade (`analysis/mods/compare/magenta_wash_1x_hud2425.png`). Those two are a full-screen overlay used in fades. The text "Y" is the first typed letter of the text box.
- **Loader check:** a replacement nutexb with a bad footer, size or mip table is skipped with `[mods] skipped ...: invalid nutexb (reason)` and the original loads (`nutexb_problem`, `runtime/mods/mods.cpp`). All 28,369 original nutexb files and the packs pass the same rules in Python; the C++ was syntax-checked with clang only.
- **4K check (2026-11-12):** `hd-florio` at `SNAP_RENDER_SCALE=2` (3840x2160, groundB 8192) loaded 28 of 28 and did not crash. The pack is copied to `%APPDATA%\SnapRecomp\mods\hd-florio`. Cross-run frame pairing at 4K failed (image difference too high), so only unpaired crops exist in `analysis\mods\compare\4k\`.
- **Pikachu pack and the hub crash (QA bug B1), result 2026-11-13: NOT size related, no pool sizing exists.** Texture pools are made per texture by the game from the data size (`MemoryPoolInitialize size=0x556000` for a 2048 body_nor, 0x2ac000 for a 2048 BC1, 22 MB at 4096), so a larger replacement just gets a larger pool. Clean runs with `hd_pikachu` (9 `[mods] hit` lines each, hub route `qa_hub1`, `SNAP_FAST_BOOT=1`, build `build\w\b1`): 1024 `2-20261113_085345`, `0-20261113_091651` (plus `1-20261112_223017` with `SNAP_MODS_DELAY_MS=3000`); 2048 `1-20261112_213201`, `2-20261112_215952`; 4096 `2-20261112_213825`, `1-20261112_222748`, and 4096 at `SNAP_RENDER_SCALE=2` `2-20261112_230507`. Across all archived runs: 222 clean runs with the 22 MB textures vs 3 crashes (about 1%), and a run with no mods at all (`2-20261112_170915`, slow, 447 s) crashed with the identical stack, so the crash is a rare guest-side race that the mods only make a little more likely (slower loads). Signature: host null read in a guest variant visitor at `main+0x95d500` (the crash is the return after `main+0x95d548 bl 0x346de0`, a generation-checked handle lock that returns null), called from `0x95e14c`/`0x95ed48`/`0x345c24` (model instance setup `0x32e3c0`); just before it, three `BufferInitialize: builder has no memory pool (pool=0, off=0, size=0x26640...)` errors: the game passed a null pool for the model's mesh buffers, i.e. a stale resource handle in the guest, not our allocator. Not fixed (no reproducer: about 1 in 100 runs; the guest chain at the null-pool call is not logged). `make_pikachu_packs.py --max-edge` defaults to 4096 again. Pikachu was not shown on screen: no test route reaches a scene with it. `runtime/os/os_memory.cpp` now logs `[os-mem]` lines when the guest's heap or page allocation fails (none seen).
- **Loader cost:** replacement files are now read and validated once and shared (before, each hit re-read the file from disk). Present count at 125 s on `qa_lab1` with other workers running: no mods 6090, owner set plus hd-florio 5550, old loader with the same set 6330. The reported 15x slowdown did not reproduce with the current owner set.

## Same-frame before/after: `SNAP_MODS_AB=<P>` (test aid, 2026-11-12)

Cross-run pairs of the HD packs failed (route timing shifts with PC load), so the packs can be toggled inside one run. Off when unset, no effect on normal play.

- Set `SNAP_MODS_AB=<P>` and a mods folder (`SNAP_MODS_DIR`). While (present count / P) is odd the draws sample the ORIGINAL texture, otherwise the replacement. With `P=1` and `NVN_FRAME_DUMP_AT=N,N+1` the two dumps are consecutive presents (same pose, only the texture differs). In the dumps the odd-numbered present showed the replacement (the dump at present N shows the frame drawn under count N-1), so check which of the pair is sharper before labelling.
- How: when a replaced nutexb is served (`runtime/mods/mods.cpp`, `ab_register`) the host also reads the original file from `SNAP_MODS_AB_ORIG` (default `extracted/drp-v262144`, same relative path as in the pack). On the first draw that samples a texture (`runtime/nvn/nvn_draw.cpp`, `AbSubstitute`, called from `ResolveHandle`), `snap_mods_ab_original` finds the pair by comparing the first 4096 bytes of the guest's texture with the replacement (same width, height and mip count), and a second `TextureImpl` is built from the original data (its own width, height and mips, `packaged` data, uploaded once like any static texture). The descriptor then picks one of the two images per draw. Both stay resident; nothing is re-uploaded. `TextureImpl::abAlt/abTried` in `nvn_internal.h`.
- Limits: 2D textures only; a replacement whose format differs from the original is skipped; the original files must exist under `SNAP_MODS_AB_ORIG`. `SNAP_MODS_AB_LOG=1` logs each registration and near-miss.
- Dump 4K frames with `SNAP_WINDOW_W=3840 SNAP_WINDOW_H=2160 SNAP_RENDER_SCALE=2` (the scale is relative to 1080p) and `NVN_FRAME_DUMP_FULL=1` (the default dump is half size).

## Material re-texture: authoring rules (owner feedback 2026-11-12)

Owner on the first pass: "Magicarp texture/relief kinda creepy. You need to be more bespoke and really make it fit rather than just slathering weird scales over it everywhere." Cause: one dense hex-cell pattern over the whole body (lips and fin edges included), cells far smaller than the scales the artist painted. Rules for every material texture from now on:

1. **Refine what the artist painted.** Find each texture's own motifs (painted scale outlines, fur strokes, seams, cracks, rib lines, markings) and add detail at their scale and direction, so the HD texture is the same design seen closer. Do not introduce a pattern the original does not suggest.
2. **Bespoke per species.** One spec per species, `scripts/hd/species/<species>.json`, with hand-placed regions (UV island ids, Lab colour ranges, UV polygons, distance to the island edge), each with its own treatment, detail size in texels at 1024, amplitude, direction, and explicit exclusions (lips, mouth, eyes, fin borders, claws, cheeks, markings). First match wins, so exclusions come first. No global pattern.
3. **Real-world logic per region.** Fish: large overlapping scales following the painted ones, rear edge slightly raised, smooth glossy lips, fins with ribs only on the painted rib lines. Fur: soft, short, flow from the painted strokes, longer and softer at ear edges, cheeks untouched. Rock: segments with a bevel and sparse chips at edges, not brushed metal. Shell: scutes with grooved seams and faint growth lines. Silhouette and colours must read exactly like the original; detail shows only up close (Detective Pikachu style).
4. **Restraint.** Low amplitude by default. Nothing that reads as holes, cells or bumps clustered densely on skin (trypophobia). Large forms first, then subtle micro detail.
5. **Judge every output** with the sheet against the original; if a region looks worse or uncanny, turn it down or off.

### Second pass (2026-11-12/13): `species_author.py`

`scripts/hd/species_author.py author <species> [--debug DIR]` reads the spec and writes `x4_mat` (4096 master) and `x1_mat` (the master area-downsampled to the original size, normals renormalised; the detail now sits at the painted scale, so it survives at 1024 without the old "3x coarser" trick) into `build/hd/cache/<asset>/textures`, registered in the manifest. `pack <species> --root DIR --pack NAME` packs only the maps the material binds (`in_game` in the spec). `scripts/hd/mat_preview.py <species>` writes `analysis/hd/mat_<species>.png`: two angles and a close-up, each as original | **new, in game today** (only the maps the shader binds) | new, full (adds cache-only maps such as a normal for a body without a normal sampler; needs the material-swap work), plus 1:1 crops at 4096 of the original (Lanczos), the new `_dif` and `_nor`. The lit render is an approximation of the game shader.

Treatments (all read the painted feature first): `scales_lattice` (Voronoi of the staggered lattice that reproduces the painted scale column; height rises toward each scale's rear edge), `fin_membrane` (black-hat rib lines -> raised ribs, slight sag and lighter membrane between), `fur` (strand direction from the structure tensor of the painted strokes, gravity guess only where the paint has no direction; half the strand field is the painted stroke sharpened along the flow; optional longer/softer band at the island edge), `skin` (the painted grain only), `plates` (painted seams -> groove, bevel, dome; optional growth lines, sparse edge chips, painted-mottling grain; seam specks shorter than `min_area` texels are dropped, because a speck turned into a ring of growth lines on Squirtle and into round pits on Steelix), `none`. Any region can add `crisp_lines` (the painted line kept, its profile narrowed at 4096) and `par` targets. Bodies whose normal does not load get a faint baked cavity in `_dif` (`bake_ao`) and the cavity in `_par` G.

| species | regions and what was done | loads in game today |
|---|---|---|
| Magikarp | Painted: one column of 5 pentagon scales behind the head line (row 48 texels, tip 41, corner 26). Large scales continue that column toward the tail with half-row stagger (column spacing 38.4 texels derived from the painted zigzag), rear edge raised, tilt 0.085; head smooth (painted grain only); lips gloss only; mouth, whiskers, crown fins, fin rays and fin borders untouched; fin ribs raised on the painted rib lines only; the fine 12-texel `_par` sparkle lattice untouched | `_dif` (faint scale shading) + `_par` (AO, lip gloss); `Body_nor` cache-only |
| Pichu | Fur from `_par` G strokes, 6 texels long; black ears/collar/tail are fur (first pass made them pored skin); ear edge band 1.6x longer and softer; toe lines excluded; Cheek upscale only | all (`_dif`, `_nor` BC5, `_par`) |
| Pikachu | Fur from the painted `_dif` grain; ears with an edge band; tail shorter and flatter; stripes and ear tips kept as fur markings; Cheek upscale only; no `_par` written (the flocky shader binds none) | `_dif` + `_nor` |
| Eevee | Short fur on head/body/legs, long clumped locks on the ruff and tail tip, very short flat fur inside the ears; nose and paw-pad dots and painted toe marks excluded | all (`_dif`, `_nor` BC5, `_par`) |
| Squirtle | Skin: painted grain only, slight gloss; carapace: grooved painted seams, dome per scute, faint growth lines near the seams; plastron flatter; rim and mouth untouched | `_dif` (crisper seams, faint cavity) + `_par`; `Body_nor` cache-only (material `BodyA`, shader `1731682751`) |
| Steelix | Stone blocks: bevel into the painted light grout, irregular edge with sparse chips, painted mottling as low grain, rougher `_par`; mouth and teeth untouched; emissive and flowing-light maps untouched | `_dif` + `_par` R/G/B (BC1, no metallic); `Body_nor` cache-only |
| Bidoof (batch 2) | Short soft fur along the painted grain on all brown and cream fur (longer at island edges); nose and paw pads smooth skin (painted grain only); mouth and incisors untouched (incisors slightly glossier). Verdict: good, close to the original in silhouette and colour, visibly finer fur in close-up | `_dif` + `_par`; `Body_nor` cache-only (shader `3436703351`) |
| Snorlax (batch 2) | Teal: short dense fur along the painted strokes; belly and face: very soft short fur at a third of the teal strength (first try at half strength added a speckled, dirty look and was turned down); brown sole pads smooth skin; claws untouched apart from gloss. 2048 `_dif` -> 4096 master (2x). Verdict: teal good, belly now almost the original, which is right | `_dif` + `_par`; `Body_nor` cache-only (shader `2887235398`) |
| Flareon (batch 2) | Orange: short fur; cream ruff, head tuft and tail: long clumped locks, longest at the jagged lock edges; navy inner ears very short flat fur; nose and toe lines excluded. Verdict: good, the strongest change in this batch; the ruff reads as fur locks, colours unchanged | `_dif` + `_par` (512); `Body_nor` cache-only (shader `2455468789`) |
| Charizard (batch 2) | Skin: scaly-smooth, painted grain as fine pebbling, slight sheen; belly smoother; wing membrane: the painted crackle veins (half a texel wide, 2-4 L contrast) could not be traced as lines (thresholding gave isolated blobs), so they are lifted only as much as they are painted (fine high-pass of the paint) with a slow undulation; mouth, teeth, claws untouched; fire `FireA/FireA2/FireB` and `Eye` crisp upscale only. Verdict: in game almost unchanged (crisper, slight sheen); the cache-only normal reads as pebbled leather | `_dif` + `_par`; `Body_nor` cache-only (shader `2185982105`) |
| Gengar (batch 2) | Mostly smooth: painted mottling as very low soft relief plus a fine velvety micro-grain; rougher and less specular (reads soft, not plastic); tongue wetter; mouth and teeth untouched. Verdict: subtle and right; the visible change in game is the softer sheen | `_dif` + `_par`; `Body_nor` cache-only (material `BodyA`, shader `2972460439`) |
| Bulbasaur (batch 2) | Skin: smooth moist amphibian, painted grain only, slight gloss, spots kept as markings; bulb: its painted light streaks are broad (no traceable vein lines; the line tracer found none), so they are lifted only as much as painted, a little leaf sheen; mouth and claws untouched. Verdict: faithful but a small change; in game nearly the original | `_dif` + `_par`; `Body_nor` cache-only (material `BodyA`, shader `2455468789`) |
| Machamp (batch 2) | Skin: soft grooves only along the painted muscle strokes, painted grain, faint sparse pores (normal only); belt, buckle, studs, trunks, crests, lips and mouth untouched; `_par` BC3 alpha kept as authored. Verdict: faithful, no new lines; in game nearly the original (the pores live in the cache-only normal) | `_dif` + `_par`; `Body_nor` cache-only (shader `4247236603`) |
| Skarmory (batch 3, metal) | Polished steel: painted grain only (very low) and the painted panel shading; roughness down to about 0.25 and spec mask up 10-12% on plates, dark neck/thigh metal and wing blades (blades smoothest); red wing undersides the same painted metal; beak, claw tips and mouth untouched. `_par` is BC3 with authored alpha (about 87 body, 255 wings) and the wing shader `2752675853` carries `metal_coef`/`Metal_wing`: alpha kept exactly as authored, not raised (which shaders read `.w` cannot be checked locally; only 7 shaders are decompiled). One spec covers the three materials sharing `Body_dif` (`Body`, `wing`, `wingB`). Verdict: faithful; in game a modest tightening of the highlights | `_dif` + `_par` (BC3, alpha kept); `Body_nor` cache-only |
| Chandelure (batch 3, metal) | Dark lacquered iron: painted grain only, painted white glints kept crisp, roughness to 0.25, spec mask up 10%; glass head (Glass/Glass2 island) and flames (`Fire_emi`, `Fire_emi2`) crisp upscale only. `Body_par` is BC1, so no metallic can be written: roughness and spec mask only. A BC3/BC7 `_par` alone would very likely not carry metallic: the body shader `2613379960` has no metal parameters (FresnelColor, innerColor, sssContrast); it needs a material swap to a metal shader such as `1096415554` (reads `paramsTexture.w`, `metal_coef`). Verdict: in game almost the original (near-black arms show little gloss change) | `_dif` + `_par` (BC1); `Body_nor` cache-only |
| Graveler (batch 3, rock) | Granite: the painted grain and the painted crater rings and blotches lifted exactly where painted (high-pass at 3 texels), rougher (R to 0.85) and less specular; fingertips lower; mouth untouched. Verdict: good; reads as matte stone in game, the cache-only normal gives real pitted-granite relief without new pits | `_dif` (baked cavity 0.35) + `_par`; `Body_nor` cache-only |
| Carbink (batch 3, crystal + rock) | Rock: the sparse painted cracks as soft grooves on the same lines, mottling as low grain, rougher; white collar smoother and satin; diamonds (`DiamondOuterRefle`, own cube map): no relief, painted facet lines kept crisp, `_par` untouched (BC3 alpha 16 kept). Verdict: faithful, small change in game; the crease detector also grooves the striped gutter padding next to the diamonds (invisible, cache-only normal) | `_dif` + `_par`; `Body_nor` cache-only |
| Heracross (batch 3, glossy shell) | Hard beetle chitin: painted speckle as a very faint grain, the painted segment lines as clean shallow grooves, roughness to about 0.2 and spec mask up 12% (lacquer-like); claws, horn tips and wings (Wing binds only `_dif`) untouched. Verdict: good, clearly glossier in game while colours stay the same | `_dif` + `_par`; `Body_nor` cache-only |
| Foongus (batch 3, plastic-like) | Ball-shaped cap: almost no relief, band edges crisp, roughness to about 0.22 and spec mask up 12%; pink underside and white fungal body matte, painted fibre streaks as low relief. Verdict: the cap now reads as plastic in game, but under the new gloss the artist's painted dark speckle on the red reads more like dust; a clean plastic would need that speckle softened in `_dif`, which changes the painted art, so it was left for the owner to decide | `_dif` + `_par`; `Body_nor` cache-only |
| Goodra (batch 3, slime) | Wet slime: the dense painted pebble cells are deliberately NOT raised (trypophobia rule); only the broad painted shading as a very soft undulation, roughness to 0.15 and spec mask up 15%; antenna pieces glossy, untouched otherwise. Verdict: faithful; the original was already glossy, so the change is small | `_dif` + `_par`; `Body_nor` cache-only (material `BodyTattu00`) |
| Lapras (batch 3, wet skin + shell) | Skin: wet smooth marine skin, painted grain only, roughness to 0.25 and spec mask up 12%; cream belly the same; shell (`BodyB`): the painted cracks as soft grooves, painted knobs and rim lifted as painted, satin; mouth untouched. Verdict: good; the skin is glossier in game, the shell reads as a separate hard material in the cache-only normal | `_dif` + `_par`; `Body_nor` cache-only |
| Vivillon (first courses) | Wings (`wing_scales`, new): rows of small shingle scales (3 x 2.2 texels at 1024) in a polar lattice around each wing root where the painted veins meet, tips outward, random row phase and a slow row waviness (a strict half-stagger read as twill when magnified), per-scale tone jitter 2.4%, scales tucked under the painted veins, veins kept crisp and slightly raised, velvet-matte `_par`; head/thorax short fur along the painted strokes; antennae, legs, pattern-square particles untouched. Verdict: at 1:1 it reads as a butterfly wing; at pack size it is the painted powder with faint rows; the CPU preview (no mipmaps) shows the 4096 scales as twill/moire, which mipmapping should remove (not checked in game) | `_dif` + `_par` (512); `Body_nor` cache-only (Body and Wing share one set) |
| Dodrio (first courses) | Down on body and heads and the black lower body (short fur along the painted strokes); tail plumes and crest as long thin barbs along the painted streaks; legs and necks: their painted reticulation as the only micro relief (no new cells); beaks smooth, glossier; claws, tongues, mouth parts untouched. Verdict: faithful and subtle; the body highlight is a little softer | `_dif` + `_par`; `Body_nor` cache-only |
| Grookey (first courses) | Short fur along the painted strokes on green, brown and orange fur (four materials share the set); face patch shorter fur; muzzle is bare skin (grain only, gloss kept); head leaf: grain only, crisp midrib, waxier; stick, mouth, tooth untouched (no wood grain was painted, none added). Verdict: good, visibly furrier up close, colours unchanged | `_dif` + `_par`; `Body_nor` cache-only (Body binds the dummy normal) |
| Scorbunny (first courses) | Short fur along the painted strokes, longer at ear tips and tufts; on the white the albedo strands are capped at 1% and the baked cavity at 0.12 (a first try at 2% made the white look grey); yellow ear marks, nose, band, mouth untouched; toe and finger lines crisp. Verdict: clean white, fur only up close; ears improved most | `_dif` + `_par`; flocky shader keeps `dummy_flocky_texture` as normal, `Body_nor` cache-only |
| Wurmple (first courses) | Red skin: the painted wrinkle crackle as the only micro relief, artist roughness kept (a rougher `_par` spread the highlight over the painted sparkle and read as speckles); belly: soft grooves exactly on the painted fold lines; spikes glossier keratin; prolegs untouched. Verdict: faithful; change is small (crisper folds) | `_dif` + `_par`; `Body_nor` cache-only |
| Pinsir (first courses) | Body and legs: chitin plates split on the painted segment grooves (narrow groove, soft bevel, gentle dome), painted mottling as fine grain, satin; limbs grain only; horns, spikes, claws smooth glossier keratin, the painted horn pores stay paint (no pits); teeth, mouth untouched. Verdict: faithful; in game nearly identical (crisper grooves), the relief is in the cache-only normal | `_dif` + `_par`; `Body_nor` cache-only |
| Wingull (first courses) | Wing and tail: long thin barbs along the painted streaks, low relief; head and body: smooth contour feathers, long soft strands, albedo change under 1% (a first try made the white look hairy); beak glossier; feet grain only; mouth untouched. Verdict: clean, subtle | `_dif` + `_par`; `Body_nor` cache-only |
| Crabrawler (first courses) | Shell: groove and soft bevel only on the painted panel outlines, gentle dome, painted mottling as faint grain, satin; cream crest and joints glossier; claw opening and dark patch untouched; no granules (none painted). Verdict: faithful; in game nearly identical (crisper outlines) | `_dif` + `_par`; `Body_nor` cache-only |
| Bounsweet (first courses) | Rind: painted grain only, a little glossier; white body matte grain; sepals waxy grain with a faint undulation; no pores (none painted; the dotted look in the preview highlight is the artist's own `_par` sparkle); nub and feet untouched. Verdict: faithful; small change | `_dif` + `_par`; `Body_nor` cache-only |

First-course batch (2026-11-13): species from Florio Nature Park, Lental Beach and Lental Jungle (course map from the `LV_MAP_*.levelb` string tables, local note `analysis/hd/course_species.md`), added with `build_cache.py --variants none`, authored with `scripts/hd/species_author_ext2.py` (species_author + `species_author_ext.py` multi-material sets + the new `wing_scales` treatment in `species_ext_wing.py`). Every body lacks a usable normal slot, so in game today only `_dif` and `_par` change; fur and plate relief stay faint until the material-swap work. `mat_preview.py` samples the 4096 maps without mipmaps, so very fine detail (wing scales, `_par` sparkle) aliases in the panels; compare at `--variant x1_mat` for the pack-size look.

Batch 3 (2026-11-13, owner asked for metal, plastic-like and organic): species picked from courses outside Florio and the other texture workers' lists, added to the cache with `build_cache.py --variants none`. A spec set can now cover several materials that share one texture set (`"mat": ["Body", "wing", "wingB"]`); island ids follow mesh order. Every batch-3 body again lacks a normal sampler, so the visible in-game change is `_par` roughness and spec mask (gloss) plus the faint cavity in `_dif`. Metallic: only Skarmory and Carbink have BC3 `_par` (alpha kept as authored); BC1 bodies get roughness/spec only.

Batch 2 lesson: every batch-2 body lacks a normal sampler, so in game only the albedo strands/cavity and the `_par` roughness change; skins (Charizard, Bulbasaur, Machamp, Gengar) will look almost unchanged until the material-swap work lets a normal load. Painted vein or crackle lines fainter than about 5 L do not trace as lines; use the paint-proportional `skin` high-pass on them instead of `fin_membrane`.

### In-game review at 4K (2026-11-14, worker hdpoke)

Packs: `scripts/hd/make_species_pack.py <species> --root build/mods_hdpk --max-edge 4096` (x4_mat, original nutexb format and sRGB flag, full mip chain, 4096 cap), one folder per species for all 38 specs, plus `build/mods_hdpk/mods.json` enabling all of them (982 MB). Routes `host/routes/hdrev_<course>.txt` (generator `scripts/hd/review/mkroutes.py`): menu navigation by `wait ui=` lines, start the course, ride with no shutter and no camera input, `end @f9000`. Runner `scripts/hd/review/run_hd.ps1`: `SNAP_RENDER_SCALE=2` at 3840x2160, `NVN_FRAME_DUMP_FULL=1`, `SNAP_NEO_SPEED=3`, `SNAP_MODS_AB=1` and `NVN_FRAME_DUMP_AT` pairs (N, N+1) every 100-120 presents from the expected ride start, so each pair is one original and one HD frame one present apart (the HD one is picked by more fine detail where the two differ, `scripts/hd/review/crop.py`).

What did not work: pausing the ride to get a frozen scene (the pause menu blurs and dims the 3D view); a 1-present pair is not frozen either (wind, idle animation and the rail move a few pixels), so a pixel diff cannot localize the Pokemon and crops were picked by eye from thumbnail grids. Each 4K BMP dump stalls about 3.5 s. `wait ride_end` fires at rail stops mid-course (Florio day stopped at progress 43, beach at 50), so the routes now just ride until the timeout. My course guesses were partly wrong: `R x5 U` and `R x5 U U` both reached Outaway Cave (Gengar, Goodra, Carbink, Beartic textures load there but none came on screen), `R U` is Mightywide River (none of the 006 species).

Grades (contact sheet `analysis/hd/review/hd_pokemon_ingame_4k.png`, left original, right HD, consecutive frames):

| species | where seen | grade |
|---|---|---|
| Aipom | Jungle, head about 400 px | good: finer fur on purple and cream, colours unchanged |
| Torterra | Florio night, shell fills the frame | good: crisper shell grain, subtle |
| Dodrio | Florio day, mid range and point blank | good at mid range; too strong at point blank (the `_par` G fur cavity and baked cavity read as darker, streaky down). Fixed per the rules: `down`/`lower_body` `cav` halved, `alb_amp` x0.6, `bake_ao` 0.3 -> 0.15 (mean `_par` G drop 6.9 -> 4.1); the fixed version is NOT re-checked in game |
| Pichu, Grookey, Vivillon, Bouffalant, Machamp | Florio day, beach | no visible change at the sizes seen (150-400 px; Bouffalant close but its afro barely changes) |
| the other 29 | | not on screen in these rides (Wurmple, Magikarp, Snorlax, Heracross, Scorbunny and Bidoof load in Florio; Pikachu and Eevee load at night; the volcano, snow and ruins species need routes that reach those courses) |

Offline check of how much each pack changes (`scripts/hd/review/texstat.py`, 4096 x4_mat vs a Lanczos upscale of the original): albedo RMS 0.2 to 3 levels, mean within 0.5, so the visible in-game change comes almost only from `_par` (fur species lower G by 7 to 11 on average, metals/plastics move R by 20 to 40). That is why most species read as unchanged until a normal map loads.

Installed (2026-11-14, owner's game not running): `%APPDATA%\SnapRecomp\mods\hd-pokemon` (86 MB: `CH_011_00_0_A00_Body_dif`, `CH_087_00_0_A00_BodyA_dif/_par`, `CH_087_00_0_A00_BodyB_dif/_par`), enabled in that folder's `mods.json` (inserted after `hd_pikachu`; the old file is in `build/hd/owner_mods.json.20261114.bak`). Everything else stays in `build/mods_hdpk` and the cache.

### Material swap: options (2026-11-14)

A body with no normal sampler can get one; `docs/creature-materials.md` "Material swap mod: result" proved it on Dodrio with a test pattern. Options, cheapest first:

1. **Data-only material mod (exists: `scripts/hd/make_material_mod.py`).** Re-point the `.matinstb`/`.numatb` to the twin shader that samples `normalTexture` and add the slot. Cost: minutes per species, no code. Risk: the normal rides on a decoy entry (a variant-1 texture), so that variant shows a normal map as colour; only 302 of 490 no-normal materials have an exact-delta twin; `sssNormalMapWeight` comes from the shader default. Gotcha found today: `--entry-name` must include `.nutexb`, otherwise the loader ignores the file (`[mods] warning: ... expected <archive>.drp/<entry>.<ext>`) and the shader samples the decoy's own texture as a normal.
2. **Loader serves new entry names** (`runtime/mods/mods.cpp`, add index entries to the DRP): removes the decoys. Cost: moderate (archive index layout). Risk: moderate. Not started.
3. **Host-side shader remap at pipeline build** (`nvn_draw`/`nvn_shader`: when program X binds, use twin Y and bind a host-loaded normal at sampler b32). Cost: high, touches files other workers own. Risk: descriptor layout per draw; still needs a twin or a translator patch for the 188 without one.
4. **Translator patch** (inject a normal fetch into the translated SPIR-V of the no-normal shader). Highest cost, covers every material.

Prototype (option 1, off by default): `build/mods_matswap/hd-pokemon-matswap` for Bidoof, Dodrio and Magikarp with their authored `Body_nor` (x4_mat, 4096; BC7 for Bidoof, BC5 for the others from the template), decoys `CH_032_00_1_A00_Body_par`, `CH_007_00_1_A00_Body_dif`, `CH_015_00_1_A00_Body_dif`. The switch is the pack's entry in `mods.json`; it is not installed. No runtime code changed. In-game result: run `1-20261114_090505` (Florio day, 4K) loaded the swapped `.matinstb`/`.numatb` for all three (`[mods] hit` lines, no crash, no shader errors), but that build of the pack had the decoy files without the `.nutexb` suffix, so the loader skipped them and the twin shader read the decoy's own texture as its normal. The pack was rebuilt with the suffix; the rerun with the authored normals did not get a run slot (the owner was at the PC and slots were closed), so how the authored normals look in game is still open.

Not done: in-game check of the other 29 species and of the re-toned Dodrio; routes for volcano, snow and ruins; the normal sign is still assumed OpenGL; the first-pass `material_author.py` stays for its helpers and the old species table only.

## Material re-texture (pilot, 2026-11-12; superseded by the rules and second pass above)

Script `scripts/hd/material_author.py` (`author <asset> --scale 4|1`, `pack <asset> --root DIR --pack NAME`), preview `scripts/hd/mat_preview.py` (offline lit CPU render + sheet `analysis/hd/mat_<species>.png`). Variant `x4_mat` = 4096 master from the 1024 originals (Cheek 512 -> 2048); `x1_mat` = same pipeline at original size with 3x coarser features so the detail survives at 1024 (this is what packs use until B1 is fixed). Both are in `build/hd/cache/<asset>/textures` and registered in the manifest. Eye and Mouth: upscale only (Lanczos on straight RGBA, alpha untouched), never in the pack.

Method:
1. **Islands and flow.** UV islands from `model.gltf` (triangles joined by shared UV points), rasterized at 2048 and extended to the gutter by nearest island, so strands/streaks stop at island borders and the padding carries the island's own pattern (no bleed over seams). Some meshes sit outside 0..1 (material repeat/offset, e.g. Cheek); the UV is wrapped per triangle. Fur flow = gravity + backward (-Y, -Z) projected on each triangle's tangent plane into UV space, smoothed per island with doubled-angle averaging, with a low-frequency angle jitter; where the flow is unsure (flat top of the head) strand strength is lowered.
2. **Regions.** k-means (6) in Lab on the albedo; each cluster centroid goes through a per-species colour rule (`SPECIES` in the script) to a class (Pichu black = smooth_skin per catalog, rest fur; Magikarp orange = scales, cream/white = smooth_skin; Steelix = metal, near-black = none). Class masks are blurred 1 px so blends do not hard-edge.
3. **Per class.** fur: two-scale line-integral-convolution strands (fine + clump) along the flow; albedo gets multiplicative luminance variation (about 4%) plus root darkening/saturation (gamma > 1 in crevices), colors locked to the original low-frequency (Gaussian sigma 2.5 px at 1024, ratio clamped 0.8..1.25); normal = original XY + strand gradient XY (std 0.08), clamped to length 0.9; _par R pulled 40% toward 0.72 with strand variation, G times (1 - 0.2 cavity), B slightly lowered in crevices. smooth_skin: pores plus two-octave noise, normal std 0.035, R toward 0.62, B x0.65 (it removed plastic gloss on the black ears). scales: jittered hex-row cells (36 px at 4096), dome + border groove, per-cell tint, G cavity 0.3 at borders, R toward 0.34, B domed. metal: brushed LIC streaks (random +-20 degree angle per island) + hammered blotches, R toward 0.2, B pulled toward 1; A = metal only where the _par format has alpha (BC3/BC7), else untouched.
4. **Channel convention written.** `_dif` sRGB, alpha kept; `_nor` in the original's format: BC5 XY (Pichu), BC7 XYZ with B copied as the original's constant 255 (Pikachu), OpenGL (+Y up) assumed for G (the originals are pure noise, so the sign could not be read from them; if lit micro-relief looks inverted in game, flip G); `_par` R/G/B linear.
5. Albedo is procedural, not SD: SD tile ControlNet re-draws pixels and risks marking drift, and the fur/scale detail needed to be strand-aligned. Mean RGB of the albedo stays within about 0.5/255 (Pichu body 3/255 in B from the crevice gamma).

Species, results (sheets `analysis/hd/mat_pichu.png`, `mat_pikachu.png`, `mat_magikarp.png`, `mat_steelix.png`; the lit render is my approximation of the shader, not the game's):
- Pichu (fur; Body dif/nor/par, Cheek dif/par): clearly furrier, colors and ear/collar markings unchanged. Seams between islands show as a change of strand direction in the flat texture, not in the lit render.
- Pikachu (fur; Body dif/nor, Cheek dif): same. The body has no `_par` slot (the fur shader `12075339` binds none), so `Body_par.x4_mat.png` is a cache-only new file for a future material change.
- Magikarp (scales): scales read as scales on the lit render, but the Body material has no normal sampler: only `_dif` and `_par` can load, so in game the relief is just the albedo shading plus AO (G) and roughness (R). `Body_nor.x4_mat.png` is cache-only.
- Steelix (metal): subtle (brushed streaks, lower roughness). Body `_par` is BC1 (no A) and the body shader `3452202033` has no normal sampler, so no metallic and no relief can load; Body_emi, flb, flc, flp untouched.
- Squirtle, rock, feather, plant, slime classes: not done.

Not done: in-game check of normal sign and strength on more than Pichu; 4096 in game (waits for B1); fur on shells/eyes; a per-region flow better than the gravity guess; SD or Real-ESRGAN base under the procedural detail; a viewer toggle beyond the manifest entry.

## Studio review (2026-11-14)

A local page where the owner compares each candidate with the original and approves, rejects or marks it "needs work". Everything it shows is game data and stays under `build/studio/` (git-ignored); the repo holds only the code (`scripts/studio/`, `tools/studio/`, three.js vendored in `tools/studio/vendor/`).

Open it: run the launch entry `studio` (or `python -E -P tools/studio/serve.py`), then go to http://127.0.0.1:8791/. The server binds to 127.0.0.1 only. Views: original left, candidate right, shared orbit and zoom, turntable, five lights, lit/albedo/normals/roughness/wireframe, close-up and mid-range, triangle counts.

Add a texture candidate (one command after the maps are in the cache as `<tex>.<suffix>.png`, as `solr` or `x4_mat` are):

```
python -E -P scripts/studio/export_studio.py add --species p097 --source-chat hd-textures --tex-suffix solr --label "Sol repaint (solr)" --notes "what changed"
python -E -P scripts/studio/export_studio.py all          # rebuild every entry in scripts/studio/candidates.json
```

`--species` is a Pokemon number (`p097`) or a `build_cache.py` alias (`florio_rock`). The exporter writes `build/studio/assets/<name>/{original,tex}/model.gltf` plus PNG maps (albedo, normal, ORM from `_par`: R occlusion, G roughness; emissive), and updates `build/studio/manifest.json`. Maps a variant lacks fall back to the original; a normal map the original material does not bind is added and flagged in the notes line. Re-adding an entry (same species, chat and kind) updates it, so rerun it after each new pass.

Read decisions: `build/studio/decisions.json` is a list of `{species, variant, verdict, comment, timestamp}`; verdict is `approve`, `reject` or `needs-work`, and the latest entry per species and variant wins. Act on `approve` before packing; read the comment on `needs-work` and `reject` and answer it in the next pass.

## Paused (2026-11-14): resume note for HD textures

The owner put HD textures and models on hold in favor of performance and bug fixes. Nothing went into the owner's `hd-pokemon` pack; its mods.json backup is `build/hd/owner_mods.json.20261114.bak`. Studio entries stay in place.

**Fur lesson.** The owner rejected every fur pass so far (Bouffalant, Pichu, Bidoof, Dodrio) as "super thick and chunky". Those passes used locks 12-26 texels wide at 1024. Fur must be fine and soft: strands about 2 texels wide at 4096, a normal tilt of 4-9 degrees on average, strands that follow the body, and the original colors. Start with one species and 2 or 3 strength variants, then wait for the owner's pick before doing the rest. `build/w/hd2/hd2_finefur.py` does this. Its flow comes from the studio's original glTF: a 3D down-and-back growth vector, projected on each triangle and mapped into UV.

Run it as `python -E -P build/w/hd2/hd2_finefur.py build/studio/assets/pichu/original/model.gltf MI_CH_006_00_0_A00_Body build/hd/cache/p006_00/textures CH_006_00_0_A00_Body fur_fine_b`. The strand fields have two octaves:
- fine: noise blur 0.7, length ±14 texels, high-pass sigma 6;
- soft: blur 2.2, length ±36 texels, high-pass 16.

| Preset | Fine normal | Soft normal | Albedo shade | Roughness | AO | Mean tilt |
|---|---|---|---|---|---|---|
| `fur_fine_a` | 0.10 | 0 | ±2.5% | ±6 | 0.04 | 4.3° |
| `fur_fine_b` | 0.16 | 0.05 | ±4% | ±9 | 0.06 | 6.2° |
| `fur_fine_c` | 0.24 | 0.08 | ±5.5% | ±12 | 0.08 | 8.9° |

**State by species** (studio verdicts in `build/studio/decisions.json`):

| Species | Candidate | State |
|---|---|---|
| Pichu (p006) | `tex-fur_fine_a/b/c` | In the studio, waiting for the owner's pick. The body has a normal slot in game. |
| Bouffalant (p097), Bidoof (p032), Dodrio (p007) | solr / x4_mat fur | Rejected. Redo with the Pichu pick. |
| Grookey (p112), p073 | x4_mat fur | Not judged. Same lock scale as the rejected passes, so expect a redo. |
| Magikarp (p015), Heracross (p003) | solr scales / chitin | Not judged. The body has no normal slot in game, so it needs the twin-shader swap (`build/w/hd2/mods5/hdsol4`). Neither appears on Florio. |
| Torterra (p087), p016 | x4_mat | Not judged. |
| Vivillon, Machamp | none | Parked. |

**Pipeline used for the Sol candidates.**
1. Sol paints 2048 maps (`build/w/hd2/sol/<species>/`).
2. `hd2_upsol.py` runs Real-ESRGAN x4plus, scales down to 4096 with AREA, and locks the low frequencies to the Lanczos source (`.solx`).
3. `hd2_restore.py` puts back Sol's grain that ESRGAN smoothed away (`.solr`).
4. `hd2_detail.py` adds the optional detail pass.

The maps land in `build/hd/cache/p<num>_00/textures/` as `<tex>.<suffix>.png`.

**Ride stop with rom-layer archives.** Every run with a pack `romfs/` layer stalled before the ride (runs 3-5). The same packs without the layer started fine. The likely cause was that new archive entries were named without their extension (`..._Body_nor` instead of `..._Body_nor.nutexb`). `scripts/hd/drp_add_entries.py` now keeps the extension and puts new textures ahead of the numatb and matinstb in each `MI_*.drl` group. The repacked archives under `build/w/hd2/mods*/hdmat*/romfs` were rebuilt with `build/w/hd2/hd2_refix.py`. This is not confirmed in game yet. To confirm it, run:

```
powershell -NoProfile -File build/w/hd2/hd2_queue.ps1 -Route hdrev_florio_a -Start 6150 -Mods <repo>\build\w\hd2\mods5 -Out <repo>\build\w\hd2\run7.out
```

The fix is confirmed when the log shows `wait 'ride' met`. If the ride still stalls, run `mods6` (the same textures with no rom layer) to confirm the archives are the cause.

**Where things live.**
- Test packs: `build/w/hd2/mods3` to `mods6`.
- Scripts: `build/w/hd2/hd2_*.py` and `hd2_*.ps1`.
- Contact sheets: `build/w/hd2/hd2_ab_run6.png` and `hd2_sol_it4.png`.
- Studio assets: `build/studio/`.
