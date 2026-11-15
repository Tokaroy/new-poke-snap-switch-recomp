# HD models: refined meshes for 4K

Goal: more triangles and rounder silhouettes on Pokémon and environment meshes at native 4K. Everything here is opt-in. Without a
model pack the game draws exactly what it always drew, and photo scoring never sees a refined mesh.

Status: milestone 1 (prototype on Dodrio, Bidoof, Bouffalant, Torterra and one Florio rock set). Results are at the end.

## 1. How the game stores and draws meshes

Container: SSBH `numshb`, version 1.10 (the format ssbh_lib reads). Each model has three files in its character archive,
`nx/Pack/Characters/master/CH_nnn.drp/SK_CH_nnn_00_0_A00{,_LOD1,_LOD2}.numshb`. Skeleton `nusktb`, materials `numatb` and
`matinstb`, model `nusrcmdlb` and `modelb` sit beside them. Environment meshes are `SM_ENV_*_LightmapUV{,_LOD1,_LOD2}.numshb`
in `nx/Pack/LevelComposite/master/LC_nnn_A.drp`.

Per object (`MeshObject`): vertex count, index count, four vertex-buffer offsets and strides, index-buffer offset, index type
(all 8,382 files use u16), a skinning flag, a bounding sphere and box, and the attribute list (usage, data type, buffer, offset).
Per mesh: four shared vertex buffers, one index buffer, and rigging groups (per object, per bone: a list of u16 vertex index
plus f32 weight, with a max-influences flag).

Layouts seen (checked over all 8,382 files with `mesh_refine check`):

| Buffer | Pokémon | Environment |
|---|---|---|
| 0, stride 48 | Position0, Normal0, map1 binormal, map1 tangent (Float3 each) | same |
| 1 | stride 8: map1 UV (Float2); some objects stride 16 with a second UV | stride 16 or 32: map1 UV, lightMap1 UV, sometimes more |
| 2 | no data in the file; the object's offset is the running sum of 32 x vertex count | size 0 |
| 3 | unused | unused |

Buffer 2 is the target of the game's GPU skinning: skinning runs in a compute shader and writes 32 bytes per vertex. Offsets in
buffers 0 and 1 and in the index buffer are running sums; index offsets are padded to 4 bytes in most files but not all.

- No morph targets. Faces change by UV offsets into an expression atlas, so new vertices only need the UV interpolated.
- LOD1 and LOD2 Pokémon bodies list every triangle 3 times in a row, same vertex order. Some LOD1 objects are byte copies of
  the LOD0 object (Dodrio's body).
- The game picks the LOD from the bounding sphere's share of the screen (model update `0x334db0`, see `docs/mod-ideas.md`
  item 27), so at normal viewing distances most Pokémon are drawn with LOD1. `SNAP_LOD_DIST` and `SNAP_LOD_FORCE0` move that.
- Each spawned model instance gets its own GPU buffer in arena 7 holding all three LODs (`SNAP_ARENA_LOG` owner
  `main+958430`). Bigger meshes therefore cost arena 7 space per instance; see section 5.

Where the renderer sees the data: indexed draws reach `DoDraw` (`runtime/nvn/nvn_draw.cpp`) as `Op::DrawIndexed` with the index
address in `a[0]`, the index type in `i[1]`, the count in `i[2]` and the base vertex in `i[3]`; the index bytes are readable
from the CPU through `Host::ResolveAddress`. The photo evaluation pass draws the same vertex and index buffers as the live view,
with its own depth target (tagged `EVDP` at creation in `TextureInitialize`, `runtime/nvn/nvn_entry.cpp`).

Replacement: the mod loader replaces any DRP entry by path and accepts any size (`docs/mods.md`). A pack places the new file at
`<pack>/nx/Pack/Characters/master/CH_nnn.drp/SK_CH_nnn_00_0_A00.numshb`. The pack root also needs a `mods.json` that lists
the pack; without one the loader enables nothing (`runtime/mods/mods_core.cpp`; `docs/mods.md` still describes the older rule).

## 2. Route: offline refinement plus a draw-range switch

Chosen: (a) offline subdivision, packed as a normal model mod, plus a small renderer switch. Not chosen: (b) runtime
tessellation.

Why not runtime tessellation: the game's shaders are translated from the Switch, and the vertex stage only exposes clip-space
outputs plus unnamed varyings, so a hull/domain stage would have to reconstruct positions, normals and UVs from shader
internals per shader. The skinned vertex format in buffer 2 is also not documented. Offline refinement needs none of that: the
game loads the larger mesh, skins the new vertices with its own compute pass and its own weights, and shades them with its own
shaders.

Refinement (`scripts/hd/mesh_refine`, Rust, uses ssbh_lib from `tools/ref`):

- PN-triangle midpoints, 1:4 split per level. For an edge P0-P1 with normals N0, N1:
  `m = (P0 + P1) / 2 - (w01 N0 + w10 N1) / 8`, `w_ij = (Pj - Pi) . Ni`. That is the PN cubic evaluated at the edge midpoint.
- An edge stays straight where the two faces use different normals (a crease or hard edge), where it is non-manifold, or where
  a normal is zero. Displacement is clamped to 0.25 x edge length.
- Midpoints are computed once per welded edge (positions welded across UV and normal seams) in a fixed vertex order, so both
  sides of a seam get bit-identical positions: no cracks.
- Skin weights of a new vertex are the average of the two welded end points (top max-influences kept, renormalized), the same
  for both sides of a seam, so seams stay closed when the joint bends (`seam_weight_mismatch=0` on every object).
- Normals, tangents and binormals are interpolated, renormalized and re-orthogonalized; UVs and lightmap UVs are linear.
- Every refinement keeps vertices 0..V-1 byte-identical, including their weights, and appends the new vertices after them.
  A u16 guard stops a level that would pass 65,535 vertices.

The index range of a refined object holds both triangle sets:

```
H (9) | original triangles | S (9) | refined triangles | [3 padding if needed] | T (9)
H = 0,0,0 1,1,1 0,0,0   S = 1,1,1 0,0,0 1,1,1   T = 0,0,0 0,0,0 1,1,1   (degenerate triangles: they draw nothing)
```

`runtime/nvn/nvn_model.cpp` checks each u16 indexed draw for H at the start and T at the end, finds S (cached per index
address), and narrows the draw:

- the photo evaluation pass always gets the original set;
- every other pass gets the refined set;
- `SNAP_MODEL_REFINE=0` draws the original set everywhere;
- `SNAP_MODEL_AB=<P>` (falls back to `SNAP_MODS_AB`) draws the original set while (present / P) is odd.

A draw that is not a refined object costs one 18-byte compare. Without a pack nothing changes. With the switch absent (an older
build) a refined file draws both sets on top of each other, which looks like the refined mesh.

Why the score cannot move: the evaluation pass draws indices that are byte-identical to the original file's, over vertices
0..V-1 that are byte-identical with byte-identical weights. The new vertices are never referenced by the evaluation draws. The
skinning pass also writes the new vertices, but each vertex's output depends only on its own input and weights, so the original
vertices' skinned positions are unchanged. The bounding sphere and box are kept as they were, so LOD choice and culling do not
change either.

Tool commands (build with `CARGO_TARGET_DIR` under `build/`):

```
mesh_refine refine <in.numshb> <out.numshb> [--levels N] [--clamp F] [--only name#sub]... [--min-tris N]
mesh_refine verify <original.numshb> <refined.numshb>    # original range, vertex bytes, weights, bounds; prints VERIFY OK
mesh_refine stats|check|info <files...>                  # faceting stats, layout invariants, headers
mesh_refine strip <refined> <out> [original]             # one set only, for offline viewers
```

## 3. Notes for the HD texture work

- UVs of the original vertices are untouched and new vertices interpolate them linearly, so every existing texture, including
  the HD `_dif`, `_par` and authored `_nor` maps, still lines up.
- Normal maps were baked against the low-poly surface. On a refined mesh the interpolated normals already carry the curvature the
  bake was compensating for, so a strong `_nor` can double the shading of rounded areas, most visibly near silhouettes. Re-baking
  against the refined mesh (high = refined LOD0, low = refined LOD0) removes that. Until then, keep HD normal maps on refined
  species mild.
- Tangents are re-orthogonalized per new vertex from the interpolated normal, so tangent-space maps stay valid.
- Lightmaps on environment meshes are baked per object; the refined rock keeps the lightmap UVs (linear), and the small
  silhouette change does not move baked shadows noticeably at the tested distances.

## 4. Building a pack

```
build/w/<you>/pack/
  mods.json                     { "packs": [ { "id": "mdl-proto", "enabled": true } ] }
  mdl-proto/pack.json
  mdl-proto/nx/Pack/Characters/master/CH_007.drp/SK_CH_007_00_0_A00.numshb        (refine --levels 2, then verify)
  mdl-proto/nx/Pack/Characters/master/CH_007.drp/SK_CH_007_00_0_A00_LOD1.numshb   (refine --levels 1)
  ...
```

Run with `SNAP_MODS_DIR=<pack root>` and `SNAP_ARENA7_MB=384`. The pack is game-derived data: it stays under `build/` or
`%APPDATA%` and is never committed.

## 5. Results (milestone 1, paused 2026-11-14)

Work paused by the owner on 2026-11-14 in favor of performance and bug fixes. Nothing is installed in any player's mods.

Pack: Dodrio (CH_007), Bidoof (CH_032), Bouffalant (CH_097) and Torterra (CH_087) at LOD0 with 2 levels and LOD1/LOD2
with 1 level, plus the Florio rock set `SM_ENV_001_1_Rock1_LightmapUV` (LOD0 with 2 levels, LOD1/LOD2 with 1). Every
file passes `mesh_refine verify`. Pack size 47.6 MiB.

| Check | Result |
|---|---|
| 4K A/B, day ride (`hdrev_florio_a`, `SNAP_MODS_AB=1`, pairs p/p+1) | Refined and original draws alternate as intended; no cracks, holes, seam openings or candy-wrapper joints in the Dodrio, Bouffalant and rock pairs |
| 4K A/B, night ride (`hdrev_florio_b`) | Ran to the end of the route; Torterra pairs captured but not yet cropped |
| Photo evaluation pass | Every evaluation draw took the original set (160 and 390 draws in two `florio_eval` runs, 0 refined) |
| Vulkan validation (`NVN_VK_VALIDATION=1`) | 0 `[vk]` messages in the day ride and both `florio_eval` runs |
| `florio_eval` score with the pack | Total 1,831 (Pose 500, Size 21, Direction 261, Placement 899, Other Pokémon 150). Not yet compared: the no-pack run reached the score at 290 s, before its first screenshot at 320 s, and route timing drift (score at present 13,043 against 14,492) means the two runs photographed different moments |
| GPU cost at 4K | Within noise: +0.004 ± 0.045 ms per frame over the day ride (average frame 2.7 ms) |
| Arena 7 peak | Day 48 MiB, night 76 MiB, against 14 to 21 MiB without the pack; needs `SNAP_ARENA7_MB` well above the 112 MiB default for wider coverage |

What the A/B shows: at normal ride distances the game draws LOD1, and a 1-level LOD1 changes silhouettes only slightly
(Bouffalant horn rings and back line, Dodrio legs). The clear gain is on close LOD0 views. The GPU measurement used a
1-present A/B period, which lines up with the game's 30 Hz animation work on alternate frames; short close-up windows
therefore show ±0.15 ms swings that flip sign with the timer lag. A clean number needs `SNAP_MODEL_AB=64` (64-present
blocks) with no frame dumps: `build/w/models/mdl_gpu.ps1` does that run but was not executed.

The night run logged one arena 7 high-water of 100% at present 374, before any course loaded and almost all of it
uncharged. The later peak is 19.8%. Check whether that boot transient also appears without the pack before relying on it.

## 6. Resume notes

Where things are (all local, nothing tracked except the doc, the tool source and `nvn_model.cpp`):

- Tool: `scripts/hd/mesh_refine` (Rust; build with `CARGO_TARGET_DIR=build/w/models/target`). GPU log parser:
  `scripts/hd/mesh_refine/gpu_cost.py`.
- Renderer switch: `runtime/nvn/nvn_model.cpp`, hooked from `DoDraw` (`nvn_draw.cpp`) and `TextureInitialize`
  (`nvn_entry.cpp`). Inert without a refined pack.
- Pack: `build/w/models/pack/` (with `mods.json`). Run scripts: `build/w/models/mdl_run.ps1` (4K A/B ride with frame
  dumps), `mdl_eval.ps1` (`florio_eval` with validation; `-NoPack` for the baseline), `mdl_gpu.ps1` (GPU cost).
  Crops: `mdl_crop.py`; A/B crops so far in `build/w/models/sheet/`.
- Runs: day `1-20261114_130524`, night `1-20261114_144138`, `florio_eval` with pack `1-20261114_145751`, without pack
  `1-20261114_150919` (all under `analysis/boot/runs/`).

What is left:

1. Finish the score check: run `mdl_eval.ps1` and `mdl_eval.ps1 -NoPack` with screenshots from 260 s, compare the
   totals, and confirm both runs photographed the same moment (same evaluation draw list in the `[eval]` log).
   Crop the night Torterra pairs and build the contact sheet.
2. Run `mdl_gpu.ps1` for a GPU number without the parity confound.
3. Export each refined mesh to the studio viewer (`tools/studio/`) and wait for an Approve in `build/studio/decisions.json`
   before anything goes into the owner's mods.
4. Raise arena 7 automatically when a model pack is installed, or document the `SNAP_ARENA7_MB` value per pack size.
5. Widen coverage species by species, starting with the species the game draws at LOD0 most often in photos.

Follow-up outside this work: `docs/mods.md` still says packs load without `mods.json`; the loader now needs it.

## Studio review (2026-11-14)

A local page where the owner sees the original and the refined mesh side by side (shared camera, wireframe view with triangle counts, close-up and mid-range, five lights) and approves, rejects or marks it "needs work". Game-derived output stays under `build/studio/` (git-ignored); code is in `scripts/studio/` and `tools/studio/`.

Open it: run the launch entry `studio` (or `python -E -P tools/studio/serve.py`), then go to http://127.0.0.1:8791/ (localhost only).

Add a mesh candidate from a model pack (a pack laid out as in section 4; the exporter reads the LOD0 `.numshb`, runs `mesh_refine strip` to keep one triangle set, converts it with `ssbh_to_gltf`, and writes the original too):

```
python -E -P scripts/studio/export_studio.py add --species p097 --source-chat models --mesh-pack build/w/models/pack/mdl-proto --label "PN-refined mesh" --notes "levels, clamp, what to look at"
python -E -P scripts/studio/export_studio.py all     # rebuild every entry
```

`--species` is a Pokemon number or a `build_cache.py` alias (`florio_rock`). Passing `--tex-suffix` as well adds the combined variant (new mesh and new textures). Needs `build/w/models/target/release/mesh_refine.exe` (build it as in section 2). Output: `build/studio/assets/<name>/{original,mesh,both}/model.gltf`, listed in `build/studio/manifest.json`.

Read decisions: `build/studio/decisions.json`, a list of `{species, variant, verdict, comment, timestamp}`; verdict is `approve`, `reject` or `needs-work`, and the latest entry per species and variant wins. Only pack meshes the owner approved; read the comments on the others before the next round.
