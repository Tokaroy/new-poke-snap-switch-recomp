# Level editor plan

Status (2026-11-15): owner list items 1, 3, 6 and 2 are built (see "Owner list status"). Earlier status (2026-11-14): milestones 1 to 3 work (see Milestones; new course IDs are deferred). `tools/level-editor` opens any `LC_*` course pack from the local unpacked data, lists its parts, and draws the course geometry, the camera rail, other paths and the spawn points in a 3D view. It also edits placements and the rail and exports a mod pack. Milestones 4 to 6 are plans, not built.

This tool was an owner to-do that was parked until the core game and graphics work land (AGENTS.md, "New Pokémon and courses"). Rust work on the recompiler itself is paused (owner, 2026-11-12); this editor is a separate tool and does not touch the recompiler, the runtime or `main`.

## Ground rules

- The tool reads the local unpacked data (`extracted/drp-v262144/`) and never writes to it. Later milestones write only into a mod pack folder.
- No game data, extracted assets or generated code go into the repository or into any tool output that is shared. Screenshots stay local (`tools/level-editor/shots/` is git-ignored).
- A new or changed course loads through the existing opt-in mod path. Original files are never edited. With no mod pack enabled the game is the original game.

## What a course is made of

A course is one pack under `nx/Pack/LevelComposite/master/LC_<id>.drp/` (275 packs). Florio (`LC_001_A`) is the reference course below.

| Part | Where it lives | Format | Read today | Write today |
|---|---|---|---|---|
| Course index (legs) | `LC_001_A.courseb` | FlatBuffers (`analysis/registries/schemas/courseb.fbs`) | yes: variants (legs) and their level hashes | no |
| Level list per leg | courseb variant `level_hashes` | zlib CRC32 of entry name without extension | yes: 7 of 7 refs resolve for Florio leg 1 | no |
| Environment placement | `LV_ENV_001_1_A.levelb` | FlatBuffers, version 6 | yes: 45 model instances (name, position, rotation, scale), lights, decals | no |
| Geometry | `SM_ENV_*.nusrcmdlb` + `numshb` + `nusktb` + `numatb` + `nutexb` | SSBH (ssbh_data) | yes: meshes, skeleton transforms; materials and textures not drawn yet | ssbh_data can write meshes; not wired up |
| Camera rail | `LV_MAP_001_1_A.levelb` | FlatBuffers path component | yes: 3 segments, segment 0 is 115.7 units (runtime log shows the rail at 0..116) | no |
| Spawns | `LV_MAP_*.levelb` spawn components (slot 10) | FlatBuffers | yes: 230 spawns on leg 1, with species template, position or linked path | no |
| Movement paths | `LV_MAP_*.levelb` path components (slot 8) | FlatBuffers | yes: 180 non-rail paths with point lists and event links | no |
| Character templates | `LV_MAP_*.levelb` slots 6, 9, 11 | FlatBuffers | yes: skeleton name, animation pack path (`%CharacterComposites%/Pokemon/<nnn>/...`) | no |
| Behavior scripts | `*.luo` (13 in the Florio pack) | compiled Lua 5.1 | listed only | no |
| Collision | `SM_ENV_*_Ground.DC4A8177` and `.navmshb` next to each ground mesh | FlatBuffers-like, schemas partial | no | no |
| Trigger shapes | `LV_MAP_*.levelb` component slot 7 | FlatBuffers | counted (202 on leg 1), not decoded | no |
| Lighting | `LV_ENV_*.lprb` (light probes, 16 MB), env params at levelb root slot 13 | binary / FlatBuffers | env parameter names only | no |
| Sound | `LV_SND_001_1.levelb` | FlatBuffers | counted | no |
| Cutscenes | `IGC_MAP_*.cutb` | unknown | names only | no |
| Course registration | `RouteData.rtd` in `nx/Pack/Resident/master/Resident.drp/` | FlatBuffers | schema exists in `analysis/registries` | no |
| Scoring and research | `ResearchPoint`, `GameParameter` level params, `MissionData`, `PokemonData` | FlatBuffers registries | partial schemas | no |
| Course text | MSBT in 10 languages | MSBT | outside this tool | no |

### levelb layout as decoded so far

| Root slot | Content |
|---|---|
| 1 | level name |
| 2 | entities (`components[]`, name hash) |
| 3 | scene nodes (`components`, `node_id`, `entity_hash`) |
| 6 | course parameters; field 0 is the scene node id of the camera rail (node, then entity, then that entity's path component) |
| 10 | point sets |
| 11 | species numbers used on the leg (Florio leg 1: 3, 6, 7, 10, 15, 16, 17, 19, 32, 34, 48, 61, 65, 73, 76, 81, 84, 87, 91, 95, 97, 99, 112, 113) |
| 13 | environment parameters (ambient color, fog, wind, rim light and others) |
| 14 | `Search_*` scan entries |

- A component is a table with a tag byte in field 0 and one sub-table in a higher field. The field number gives the kind: 1 light or probe, 4 transform, 5 model instance, 6 character template, 7 collision or trigger shape, 8 path, 9 character name, 10 spawn, 11 character behavior, 14 effect, 15 decal. These names are working labels from the data, not from code.
- Spawn (kind 10): field 1 position, field 2 rotation quaternion, field 3 records (field 0 = template entity hash, field 2 = node id of a path entity). 153 of 230 Florio leg 1 spawns carry their own position; the rest start at their linked path.
- Model instances name a mesh. The file is `<name>_LightmapUV.nusrcmdlb` or `<name>.nusrcmdlb` in the same pack (all 45 Florio leg 1 instances resolve this way; LOD1 and LOD2 files also exist).

Measured with the viewer: Florio leg 1 has 788,912 triangles in view with sky and lighting helper meshes hidden, and loads in about 0.6 s cold. Leg 2 (`LV_MAP_001_2_A`) has a 13-point rail of 24.6 units, 75 spawns and 44 model instances. Leg 2's rail length has not been compared with a runtime log yet.

## How a new or changed course loads at runtime

The existing mod loader (`docs/mods.md`, `runtime/mods/`) replaces single DRP entries from `%APPDATA%\SnapRecomp\mods\<pack>\<archive>.drp\<entry>.<ext>`. The hook sits in the DRP entry worker (`MAIN_INLINE_HOOKS` in `recomp/snap_gen.py`, before `main+0x5e3a58`). That gives two paths:

1. **Changed course (works with today's loader).** The editor writes edited copies of existing entries (for example `LV_MAP_001_1_A.levelb`) into a mod pack under `LC_001_A.drp\`. With the pack enabled the game reads the edited entry; with it disabled the game reads the original. Nothing in `extracted/` or the game dump changes.
2. **New course ID (deferred).** A new course needs entries that do not exist in any archive: a new `LC_<new>.drp` pack, new level entries with new name hashes, and new rows in `RouteData`, `GameParameter`, `ResearchPoint` and the text tables. The current hook only swaps entries the game already asks for. Milestone 3 step 3 was deferred on 2026-11-14 by the coordinator: the owner's priority is game performance and bug fixes, and nobody builds the loader support for now. The spec below is what the work needs when it resumes.

### Deferred: new course ID spec

Facts come from `analysis/registries/README.md` ("Course and level model", "What a mod needs to add", "Reader code"). Addresses are in the main ELF, module base 0.

Loader changes (`runtime/mods/`, `recomp/snap_gen.py`; outside the editor):

1. Serve a new archive. The DRP lookup (`0x5e31b0`) takes an entry name plus a type id (CRC32 of the extension, for example `0x37cd5f6b` = `crc32('mdfb')`). Today the hook before `main+0x5e3a58` only swaps the bytes of entries the game already found. A new `LC_<new>.drp` needs the lookup itself to succeed for an archive name that is not in RomFS: hook or wrap the archive open and the index lookup so a name listed by an enabled pack resolves to the pack's folder.
2. Serve new entries inside it. `courseb` stores CRC32 (zlib) hashes of level entry names with no extension (`LV_ENV_*`, `LV_MAP_*`, `LV_SND_*`), so the loader must answer lookups by hash for names that exist only in the pack. The pack declares them, for example in a `new_entries` list in its manifest, and the loader builds a hash-to-file table at startup.
3. Keep it opt-in. Nothing changes unless a pack listed in `mods.json` declares new archives; with no such pack the lookup path stays the original.
4. Regenerate `main` after the hook change and rerun the base-game regression routes.

Registry rows (all in `Resident.drp` or the LevelCommon registries; the editor would write these as edited registry entries, which today's loader can already swap):

| Table | Reader | New row |
|---|---|---|
| `RouteData` (`rtd`, 30 rows, ids 1..30) | `0x63d9c4` | route_id 31 or higher (ubyte), `level_pack` = the new `LC_nnn[_n]_X` name, course variant, text keys (`Place_Name.CourseNNN_Name`), map points |
| `ResearchPoint` | `0x63e55c` | same route_id, 4 thresholds (the existing ones look like 0 / 40,000 / 110,000 / 200,000) |
| `GameParameter` level params (`paramb`, 32 rows) | `0x627ec0` | `level_pack_hash` = `zlib.crc32(LC name)`, stage id, time-of-day code (27/28/29 = A/B/C) |
| `SequenceData` | `0x628024` | navigator voice entries, only if the course has any |
| `MissionData` / `mdrp` | `0x63f830` | rows with the new route_id, only if the course has missions |
| `SilhouetteData` (`silb`) | not located | rows with the new stage id |
| Text (MSBT, 10 `TextResource_*.drp`) | n/a | `Place_Name` and `Place_Info` for `CourseNNN` and `StageNNN` in every language |

Other parts:

- Save data: save file 11 stores course status as one byte at index `route - 1`. Route ids past 30 need a check that the array has room, or the course status must live elsewhere.
- Map screen: the course needs map points in `RouteData` and possibly UI map assets; the map-select UI has not been checked for a fixed course count.
- Editor side (inside `tools/level-editor/`): "Save as new course" would copy the current pack's entries under new names, write the `courseb` with the new hashes, and write the registry rows above. Proof is the same as other milestones: an in-game run that loads the new course from the map, rides it to the end and reaches photo select.

Until then, editors change existing courses through path 1.

## Placing existing species

Everything needed to place a species already in the game is in the level data the viewer reads:

- a character template entity (kind 6 skeleton plus animation pack path, kind 9 `CH_nnn` name, kind 11 behavior);
- a spawn component pointing at that template, with a position or a path link;
- optionally a path entity for its movement, with events that link back to entities;
- the species number in root slot 11, which likely drives which `CH_nnn.drp` packs are loaded for the leg.

A placement editor clones a template already in the leg (or copies one from another course's levelb), adds a spawn, and writes the edited levelb into a mod pack. Unknowns: what behavior field values mean, how a species' behavior script is chosen, and whether slot 11 must list every species used or is only a preload hint. Each is checked by an in-game run after the edit.

## Importing new Pokémon models later

A new species needs, per `analysis/registries/README.md`: `PokemonData`, `MissionData`, `RouteData` hooks, `ResearchPoint` rows, `GameParameter` level params, `SilhouetteData`, text in 10 languages, and a `CH_nnn.drp` pack with model, skeleton, materials, textures and animations. Steps:

1. **Clone first.** Copy an existing species pack to a new ID with only names changed and prove it loads, spawns, animates, scores and saves.
2. **Model import.** glTF in, SSBH out: `nusrcmdlb`, `numshb`, `nusktb`, `numatb`, `nutexb`. ssbh_data writes mesh, skeleton, material and animation files; `scripts/hd/ssbh_to_gltf` already covers the reverse direction. Textures need a NUTEXB writer (block-linear layout, BC formats); the asset tooling list in AGENTS.md already calls for one.
3. **Skeleton and animation.** The simplest legal path reuses an existing species skeleton and its animation set, so behavior scripts that name bones and animation slots keep working. A fully new skeleton needs every animation the behavior asks for.
4. **Scoring and research.** Photo scoring reads per-species data (size, pose, research entries). These rows live in the registries above and must be decoded before a new species scores correctly.
5. **Save identity.** The save stores species numbers. A new ID outside the original range must be checked against save code before release so that modded saves cannot break an unmodded game.

## Tool stack

Rust, one Cargo project in `tools/level-editor/`, own `target/` folder inside it.

| Crate | Use | License |
|---|---|---|
| eframe / egui / egui-wgpu 0.37 | window, panels, lists, input | MIT OR Apache-2.0 |
| wgpu 30 (via egui-wgpu) | 3D view, offscreen render | MIT OR Apache-2.0 |
| glam 0.34.3 | math (same version ssbh_data uses) | MIT OR Apache-2.0 |
| ssbh_data (vendored in `tools/ref/ssbh_lib`) | SSBH mesh, skeleton, material, animation read and write | MIT |
| bytemuck | vertex buffers | Zlib OR Apache-2.0 OR MIT |
| pollster | block on wgpu futures in headless mode | MIT OR Apache-2.0 |
| png | screenshot output | MIT OR Apache-2.0 |
| walkdir | model index fallback | MIT OR Unlicense |
| crc32fast | entry name hashes | MIT OR Apache-2.0 |

Reasons: egui keeps the UI and the 3D view in one process with no web stack; wgpu runs on the 4090 through DX12 or Vulkan; ssbh_data is the only open SSBH library and is already used in this repo. FlatBuffers are read by a small bounds-checked reader (`src/fb.rs`) instead of generated code, because the schemas are inferred and still change. Writing levelb later can use the `flatbuffers` crate (Apache-2.0) once field meanings are stable, or a patch writer that keeps unknown fields byte for byte.

### Current prototype

- `src/fb.rs` FlatBuffers reader. `src/level.rs` levelb parser (components, paths, spawns, templates, rail) that also records the byte offset of every editable float. `src/course.rs` course packs, legs, level hash resolution, mesh lookup. `src/geom.rs` SSBH to triangles. `src/pick.rs` ray picking and ground snapping against the course triangles. `src/edit.rs` selection, edits as float writes, undo and redo, checks, mod pack export. `src/render.rs` offscreen wgpu renderer and orbit camera. `src/app.rs` the editor UI. `src/headless.rs` the screenshot mode.
- `level-editor` opens a window. `level-editor --course LC_001_A --variant 1 --shot shots\florio_leg1.png` renders the same UI offscreen to a PNG with no window. `--summary` prints the parts with their `[level:index]` numbers.
- Editing from the window: click a Pokémon or a scenery piece (scenery is locked by default), drag it along the ground, or type numbers. Inspector buttons: Drop to ground, Face the rail, Show me. Ctrl+Z / Ctrl+Y undo and redo; G drops, R faces the rail, F frames, Esc deselects. "Save as a mod pack" exports to a folder or installs into the game's mods folder. The Advanced toggle adds raw values (byte offsets), full XYZ scale and the level file list.
- Editing from the command line: `--move L:I:dx,dy,dz` (spawn), `--move-model NAME:dx,dy,dz`, `--copy-spawn L:I:x,y,z`, `--add-model NAME:dx,dy,dz` (with `--export`), `--bend-rail I:dx,dy,dz[:soft]`, `--export DIR --pack NAME`, `--rail-view D` (rail position at distance D and the models in front of it), `--rail-info` (every rail point with spacing, turn angle and per-edge event records), `--layout` (root and table layout of each level), `--selftest` (29 checks: round trip, add, remove, rail bend and its export, ride time, sharp-bend and bunched-point checks, undo).
- Build: `cargo build --release` in `tools/level-editor` (about 7 s incremental).
- Known gaps: no textures or materials (flat colors by mesh name), no LOD switching, no collision view. Original objects cannot be deleted yet (added ones can). Pokémon that walk a route cannot be copied yet (their route node would need cloning too).

## Endgame: easy for anyone, deep for experts (owner, 2026-11-14)

The owner's goal: anyone can make their own course without reading this document, and an expert has every tool needed to build something strange and new. Both are one editor over one data model, with two layers of UI.

**Beginner layer** (default view):

- Start from a template. "New course" copies an existing course (Florio, Beach, Jungle...) as the starting point, so a fresh course always loads and plays.
- Pokémon palette. Pick a species from a picture grid and click in the 3D view to place it; it lands on the ground and faces the rail. Behavior comes from a short preset list ("wanders", "sleeps", "flies over", "reacts to fruit") built from behaviors that already exist in the game.
- Draw the rail. Click points along the ground; the editor smooths them into a rail, shows its length and a ride-time estimate, and keeps it clear of walls.
- Live checks in plain words: "Pidgey at (12, 3, 4) is under the ground", "the rail passes through a rock", "no Pokémon within view of the last 20 seconds of the ride".
- Play test. One button exports a mod pack into a private test folder and starts the game straight into that course; the default game stays untouched.
- Undo and redo for every action, autosave, and a share button that zips the mod pack (course edits only, never original game files).

**Advanced layer** (one toggle, same project):

- Property inspector with every decoded levelb field, raw values for undecoded ones, and the component kind table.
- Path editor with per-point events (trigger animations, spawn waves, link to other entities) and multiple rail segments.
- Behavior scripts: view and edit `luo` once a Lua 5.1 decompile and compile path exists; custom presets saved for the beginner palette.
- Collision and navmesh view and editing, trigger volumes, lighting and environment parameters (fog, wind, ambient, rim).
- Import: glTF meshes for scenery, new Pokémon models and animations, textures, with validation reports.
- Registry editing: course list entry, unlock rules, research entries, scoring parameters, text in all languages.
- Batch tools: a command line and a script API (Rust or Lua) for scatter placement, procedural paths and bulk edits.

The beginner layer only offers what the advanced layer has proven in game, so the aim is that a beginner never builds a course that crashes.

Built so far (2026-11-14): the Beginner and Advanced toggle, moving Pokémon and scenery with snap to ground, face the rail, undo and redo, plain-word checks (under the ground, far from the rail), and saving as a mod pack that switches itself on. The Advanced layer shows raw values with byte offsets, full scale and the level file list. Milestone 3 added the "Add a Pokémon" palette (one button per species in the course), Make a copy (Ctrl+D) and Remove (Delete) for added objects, and rail bending: show the rail points, drag one and its neighbors follow on a smooth curve, with the ride length against the original, the change in ride time in seconds (at the 0.82 units a second measured in game) and checks for sharp new bends, bunched points and rail under the ground. "Reset the rail" puts every point back in one undo step.

## Owner list (2026-11-15)

Order from the coordinator. Effort is working time including the in-game proof. Imported user assets stay local (mod packs and a local import folder) and never go into the repository.

| # | Item | Effort | Depends on |
|---|---|---|---|
| 1 | Ease of use: move and rotate gizmo with grid and angle snapping, a search box over species, scenery models and the object list, a tooltip on every control. Undo and redo already cover every edit; new actions join it. | 3 to 4 h | nothing |
| 2 | Terrain painting: raise, lower, smooth and flatten brushes on the ground mesh, and texture-layer painting. | 2 to 4 days | 2a: decode the ground collision (`DC4A8177`) and navmesh (`navmshb`) well enough to rewrite them, or Pokémon walk on the old ground. 2b: how ground materials blend layers (vertex color or textures). Lightmaps stay baked, so reshaped ground keeps its old shading until there is a bake path. |
| 3 | Object placement: click to place on the surface, a scatter brush with random scale and rotation, align to the slope. | 0.5 to 1 day | `grow.rs` clones (built) |
| 4 | Scenery import: glTF and OBJ first (`gltf`, `tobj`, both MIT/Apache), FBX later (binary only, `fbxcel-dom`). SSBH out through `ssbh_data`; materials chosen from the game's existing presets; textures through the NUTEXB writer from the HD texture work; collision from the mesh after 2a. | 2 to 3 days | New file names need the loader to serve entries that are not in the archive (deferred 2026-11-14). Without it an import can only replace an existing model's files. |
| 5 | Pokémon import: a custom model plus a rig mapped onto an existing species' skeleton, so its animations and behavior carry over. | Reskin of an existing species: 3 to 4 days. New species ID: 1 to 2 weeks. | Reskin: 4's mesh writer. New ID: loader new entries, registry rows, save identity checks (milestone 4). |
| 6 | Procedural tools: rule-based scatter by slope, height and distance to the rail or a path, with a seed. | 0.5 day | 3 |
| 7 | Random course from a seed: terrain, rail, scenery and spawn points, playable to the end. | 1 to 2 weeks | 2 and 4. First version replaces an existing course through a mod pack (Florio's slot); its own course ID needs the deferred loader and registry work. |

### Owner list status (2026-11-16)

- **1. Ease of use (done, commit 822e405).** Gizmo arrows and turn ring with grid and angle snapping, a search box over species, scenery and the object list, a tooltip on every control, undo and redo for every edit.
- **3. Placement (done, commit 679ffd4).** Place tool (click a surface), Scatter brush (radius, count, random heading and size, minimum gap, align to slope). A stroke is one undo step.
- **6. Rule-based scatter (done, commit 19681b3).** Fill the course by max slope, height band, distance band to the rail and ground-only, with a seed. In game (run `0-20261115_221657`): `lv_env_001_1_a <- fill-test (78532 bytes)`, the ride reached rail progress 30 and exited normally, and a new trunk stands on the path at rail 6.
- **2. Terrain (done).** Terrain tool (T): raise, lower, smooth, flatten, paint layer and erase layer, with size, strength and "Pokémon and scenery follow the ground". Each stroke is one undo step. Command line: `--terrain raise@x,z[,radius[,strength[,dabs]]]`.
  - In game, pack `terrain-test` (run `3-20261115_225336`, route `test/ab_florio.txt`): a hill 3 units high left of the path at (21, 12). The log shows `[mods] hit` for both ground meshes, the collision (572,504 bytes), the navmesh (85,496 bytes) and `lv_map_001_1_a` (spawns standing on the hill moved up with it). The ride reaches rail progress 30 and exits normally. At rail 15 the hill rises beside the path, with the path's dirt carried up its side: `tools/level-editor/shots/ingame_terrain_rail15.png` (local; left: the item 6 fill-test run on the original ground).
  - Layer paint, pack `terrain-rgb` (run `1-20261115_232812`): grass painted over the dirt path at (26, 22) shows at rail 15. The same paint written to the alpha channel instead (run `0-20261115_230308`) shows nothing: `tools/level-editor/shots/ingame_layer_alpha_vs_rgb.png` (local).
  - Limits: reshaped ground keeps its baked lightmap shading. Ground models outside the course pack (shared between courses) cannot be edited.
- **4. Scenery import (glTF and OBJ; commits 00ca234, b27734c and the Import panel).** A user's model becomes a new model group in the course archive, placed once, with collision. FBX is not supported yet.
  - **Where it goes.** The pack gets the rewritten course archive under `romfs\` (the mods system's existing rom layer, so the loader needed no change), the level with the new instance, and the ground collision with the new shape, both as loose files. A second import into the same pack builds on the first. Imported files stay where the user keeps them; only the pack holds converted copies.
  - **Model.** `src/scenery.rs` converts each source material's part into a mesh object with the vertex layout of a template model from the course (any model group, e.g. a rock), assigns one of the course's own game materials, and bakes the placement into the vertices. The course's own scenery is drawn this way (geometry in world space under an instance at the origin); a model moved only by its instance never draws, because the game culls by the stored mesh bounds. Each part gets its own shape name. The new instance's lightmap list (component 5 field 17) and its field 23 list get one entry per part, in model order; with lists that do not match the model, the model does not draw.
  - **Collision.** `terrain::append_shape` adds the model's triangles as one more shape to the collision file of the ground model under it, with the surface flags of the course's props.
  - **Using it.** Editor: the Import a model panel (file, name, template, material, scale, turn, height), placed at the selected object or the view's center, written into the pack named under Save as a mod pack. Command line: `--import FILE@x,y,z --template GROUP --material MI_x [--import-name SM_USR_x] [--import-scale s] [--import-yaw deg] --export DIR --pack NAME`; y is the height above the ground there. `--list-materials` lists the templates and materials.
  - **In game.** A signpost (two parts, 24 triangles each) draws on Florio (runs `3-20261116_012608` and `1-20261116_014126`). With collision added (run `3-20261116_022055`) the game loads the 574,308-byte collision file and the ride reaches the end. Two imports in one pack, signposts at (24, 12) and (30, 20) (run `1-20261116_023635`): the log shows the level (54,468 bytes) and the collision (576,128 bytes, 70 shapes) from the pack, both posts stand by the path at the start, and the ride reaches rail 30 and exits normally.
  - **Open.** The model draws dark, as if only ambient light reaches it. Pointing the lightmap UVs at the template's most upward-facing texel and using a ground template did not change that; the lightmap entry's transform or the material's lighting path are the next suspects. Not checked: something in the game bumping into the model. The navmesh is unchanged, so Pokémon can walk through it. A per-part material choice is command line only (`--material a=MI_x,b=MI_y` by source material name).
- **5. Pokémon reskin (`src/reskin.rs`).** A user's model replaces an existing species' meshes; its skeleton, animations, materials, sounds and behavior stay. A new species ID is still deferred with new course IDs.
  - **What is written.** New `numshb` files for the species' model and its LODs, with the original mesh object names and subindices, so the model files (`nusrcmdlb`), skeleton and animations need no change. They go into the pack as loose files (`<pack>\nx\Pack\Characters\master\CH_nnn.drp\SK_CH_nnn_00_0_A00.numshb`); the archive is not rewritten.
  - **Parts to objects.** Each source material's part goes to the object named in `--reskin-map source=object`, else to the object whose material has the part's material name (a model exported from the species itself), else to the species' biggest object. Objects in `--reskin-keep` stay as they are (eyes, for example); every other object with no part becomes one zero-area triangle, which draws nothing.
  - **Rig.** A glTF skin's joints map to the species' bones by name, up to four weights per vertex. A vertex with no matching joint, and every vertex of an unskinned model, follows the nearest bone the species' own mesh is skinned to. A model rigged in Blender on the species' skeleton (exported from the game files with `scripts/hd/ssbh_to_gltf`, kept local) animates fully. The vertex layout follows each original object, as for scenery.
  - **Using it.** Editor: the Reskin a Pokémon panel (file, species, objects to keep; selecting a Pokémon's marker picks its species). Command line: `--reskin FILE --species CH_nnn [--reskin-keep a,b] [--reskin-map mat=object] [--import-scale s] --export DIR --pack NAME`; `--reskin-info [--species CH_nnn]` lists species packs or a species' mesh objects.
  - **In game.** Test model: Dodrio (CH_007) exported to glTF and cut below 55% of its height, so the necks and heads are gone (2,588 of 6,408 triangles kept; local file). All 89 joints matched. Run `2-20261116_025229`, with pack `florio-m3-ride` for the Dodrio copy on the path: the log shows all three meshes from the pack (302,976 bytes each), and at rail 15 a headless Dodrio stands on the path, undistorted on its skeleton, with its shadow and normal shading. The ride reaches rail 30 and exits normally.
  - **Open.** The user's UVs sample the species' own textures; new textures go in through a texture pack (`docs/mods.md`). Bones the species lacks (a tail on a species without one) cannot be added, so such parts move rigidly with the nearest bone.
- **7. Random course (`src/procgen.rs`).** A course made from a seed is laid over Florio leg 1's first rail part and written as a mod pack. The ride plays to the end.
  - **Generator.** The course comes from `snap-procgen`, a separate program in the Rust workspace (`work-rust/crates/snap-procgen`, GPL-3.0-only). The editor never links it. It runs the program and reads its JSON (format 2): heightfield, 8-layer splat, rail samples every 5 ft, scenery by class, Pokémon spawn points by habitat. `serde_json` is the only new dependency.
  - **Mapping.** The generator's rail is scaled to the length of rail part 0 (about 0.088 units per foot). The rail keeps its point count and both ends, so the rest of the leg still follows.
    - It swings sideways with the generator's wander, up to 3.5 units, faded out over the first and last 12%. Raycasts against the course's own models limit the swing (1.6 units of clearance), so the camera does not pass through trees or cliffs.
    - Its height follows the new ground plus a small bank term: the height variation the generator's flat rail lacks.
    - Ground within 10 units of the rail takes the generator's relief, up to 4 units. A dirt path 2.2 units wide is painted under the new rail; elsewhere the generator's grass and moss pick the layer. Pokémon and scenery follow the ground, as with the Terrain tool.
  - **Scenery and Pokémon.** The course's own tree and rock groups are baked at the origin and span up to 132 units, so they cannot be copied. Trees, rocks and bushes are built as one low-poly OBJ (trunk and cones, rough lathes) and imported at export through item 4's path, with bark, hedge and rock materials from the course. Flowers are copies of the course's flower props. Pokémon spawns copy the course's own species (fliers for air spawns), 3 to 9 units from the rail, facing it.
  - **Using it.** Editor: the Random course panel (a course file, or the generator's path plus a seed and look). Then Save as a mod pack. Command line: `--procgen FILE [--procgen-opts band,swing,relief,props,spawns] --export DIR --pack NAME`. It is one undo step until export imports the scenery.
  - **In game.** Seed 17, forest, 1300 ft, pack `random-forest17`: ground reshaped at 2,202 points, rail swing up to 3.2 units and −1.1 to +2.0 units in height, 26 trees, 19 rocks, 7 bushes, 6 flowers, 11 Pokémon. Run `2-20261116_032612`: every pack file is served (level 53,572 bytes, collision 775,044 bytes, navmesh, the archive). The ride reaches rail 6, 15, 30 and 100, then the end and photo select, and the game exits normally. At rails 15 and 30 the camera runs along the new dirt path, clear of foliage and rock. The first run (`1-20261116_031316`) swung through a hedge and a cliff and led to the clearance raycasts.
  - **Open.** The built scenery draws dark (the item 4 lighting issue). The navmesh is unchanged, so Pokémon can walk through the new scenery. The new rail is shorter than the original (the leg goes from about 125 to 121.6 units), so the ride ends about 4 seconds sooner. The course's original spawns and scenery stay. Layer paint is only as fine as the ground mesh's vertex spacing. A course with its own ID waits on the deferred loader and registry work.

### Ground formats (decoded 2026-11-15, within the 1-day timebox)

- **Collision (`<ground>.DC4A8177`).** FlatBuffers root with a vector of shapes. A shape has position, Euler rotation and scale (Vec3 each), a vertex vector (Vec3 structs), a face vector (three u32 indices plus a u32 flags word) and a u32 hash. Florio's ground has 68 shapes and 13,005 triangles; the 45 shapes with no rotation or scale are ground pieces, the rest are rocks. The collision matches the drawn ground within 0.026 units along the rail.
- **Navmesh (`<ground>.navmshb`).** A FlatBuffers wrapper around one Detour tile (magic `DNAV`, version 7, 64-bit poly refs, 16-byte links), laid out as standard Detour: 100-byte header, vertices, 32-byte polygons (vertex count at +30, area and type at +31), links, detail meshes, detail vertices, detail triangles, BV tree, off-mesh links. Florio: 464 polygons, 889 vertices, quantization 3.33 (cell size 0.3). It sits a steady 0.35 above the drawn ground (Recast's detail offset).
- **Layer blend.** The ground is several stacked mesh objects, one per material. Each carries its blend in the Color1 RGB: on Florio, 1 shows the grass and 0 lets the dirt path below show through. Painting adds to the RGB. Alpha usually equals the RGB in the original data, but changing it alone shows nothing in game, so the editor leaves it alone.

### How terrain edits are written

The editor keeps a height grid and a layer grid (1 unit cells) over the ground models. On export it patches the files in place, so nothing else in them changes:

- **Mesh:** vertex positions, normals and the tangent frame, Color1 RGB. Bounds only grow. Rocks and props that share the ground model stay put: a vertex follows the ground only if it was within about one unit of it.
- **Collision:** the plain ground pieces only (no rotation or scale), with the same rule.
- **Navmesh:** vertices and detail vertices, then the tile's vertical bounds and the BV leaves of the polygons that moved (floor for the low bound, ceil for the high one). Untouched leaves keep their bytes: the game's own leaves come from Recast data that is not in the file, so they cannot be recomputed exactly (a fit explains 389 of 465).
- `--selftest` checks that an unedited ground writes back byte-identical (mesh, collision, navmesh), that a raise of 1.49 lifts the collision by 1.47 and the navmesh by 1.36, that every raised polygon sits inside its BV leaf, and that undo flattens the ground again.

## Milestones

1. **Viewer (done).** Open a course pack, list parts, draw geometry, rail, paths and spawns. Proof: `shots/florio_leg1.png` (local).
2. **Round trip and edit in game (done, 2026-11-14).** Edits patch values in place inside a copy of the original levelb, so an unedited export is byte-identical and an edited one changes only the edited floats. Export writes a mod pack (`<pack>\nx\Pack\LevelComposite\master\LC_001_A.drp\<entry>.levelb` plus `pack.json`) and adds the pack to `mods.json` in the mods root: since 2026-11-13 the loader enables only packs listed there.
   - `--selftest`: unedited export identical for all three Florio leg 1 levels; one moved spawn changes 20 bytes of LV_MAP; a spawn placed by its route moves all 9 route points; undo restores the original bytes.
   - In game, run `2-20261114_135157` (hidden, run slot): pack `florio-rock-lift` raises SM_ENV_001_1_Rock1 by 12 units and moves spawn 1:5 (CH_007) by 5 units. The log shows `[mods] hit .../lc_001_a.drp/lv_env_001_1_a` (51,780 bytes) and `lv_map_001_1_a` (289,816 bytes), and the hands-off ride reaches rail progress 30 and exits normally. At rail progress 15 the cliff behind the path is gone from view. Baseline run `1-20261114_113916` (no pack) shows the original cliff. Side by side: `tools/level-editor/shots/ingame_ab_rail15.png` (local). Input script: `tools/level-editor/test/ab_florio.txt`.
   - Moved to milestone 3 and then deferred: a new course pack ID that loads and plays start to end (spec above).
3. **Placement editing (moving, adding and rail bending done, 2026-11-14; new course IDs deferred).** Select, move and rotate spawns and model instances in the 3D view (drag along the ground, number fields, drop to ground, face the rail, undo and redo, plain-word checks).
   - **Adding objects.** `src/grow.rs` grows a levelb: it inserts one 16-aligned block right after the root table, so only the root's own offsets cross it. The entity and scene-node vectors move into the block with the clones appended; every other root offset gets the block size added. A clone copies the source entity's tables (vtables, inline bytes, offsets re-pointed to the shifted originals) and gets fresh IDs: entity hash, node ID, component IDs. `grow::verify` checks that the original content is unchanged, the counts went up by one and the new IDs are unique. In the editor an added object keeps its floats in a virtual region past the file, so add, move, remove and undo are all plain float writes. Export grows the file and checks it. Copying works for scenery and for Pokémon with their own spawn point; a Pokémon that walks a route is refused for now.
   - **Rail bending.** The rail is the path component of the entity named by course parameters s0. Florio leg 1 has three parts: 50 points (115.7 units), 2 and 4. Each edge between two points has its own event records (49 for 50 points), so moving points keeps every event on its edge. A drag moves the point fully and up to N neighbors on each side by a cosine falloff (N = 3 by default, 0 to 8 in the inspector). Points where two parts join move together.
   - In game, pack `florio-m3-ride` (run `1-20261114_151834`): Dodrio (CH_007, spawn 1:5) copied onto the path at (27.2, -0.14, 13), and rail point 8 raised 2.5 units with 4 neighbors following (ride +0.75 units, +0.6%). Route `tools/level-editor/test/ride_florio.txt` rides the whole course: `[mods] hit .../lv_map_001_1_a <- florio-m3-ride (295912 bytes)`. The first rail restarts at 116.49 (115.7 + 0.75), then leg 2 runs, `ride_end`, then photo select opens and the run exits 0. Exactly 10 photos taken (film 72 to 62, no further change). Rail progress 6, 15 and 30 were reached after 7.7, 11.6 and 19.3 s, the same as the unedited runs. Rail part 0 took 142.0 s (116.49 units) against 141.0 s (115.74 units) in three unedited full rides the same day (runs `1-20261114_123015`, `153546`, `155607`), so the speed is unchanged and the ride is longer by exactly the added length. At rail 15 the copied Dodrio stands on the path and the camera sits higher: `tools/level-editor/shots/ingame_m3_rail15.png` (local).
   - Scenery copy: pack `florio-add-rock6` adds a copy of SM_ENV_001_1_Rock1 six units above the original (run `1-20261114_154514`, route `test/ab_florio.txt`). The log shows `lv_env_001_1_a <- florio-add-rock6 (52628 bytes)`; the ride reaches rail 30 and the run exits normally. At rail 15 the copy's rock slabs hang in the sky above the path: `tools/level-editor/shots/ingame_m3_rock6.png` (local). An earlier copy 14 units up (run `1-20261114_151716`) also loaded and rode normally but sat out of frame.
   - Still to do: show collision (`DC4A8177`, `navmshb`) so placements stay on walkable ground; copy route-walking Pokémon.
   - Deferred (coordinator, 2026-11-14): new course IDs. The loader and registry changes are written up in "Deferred: new course ID spec" above; no one builds them until the owner's performance and bug-fix work is done.
4. **Species clone.** Copy an existing species to a new ID across `CH_nnn.drp` and the registries; prove spawn, animation, scoring, research entry and save in game.
5. **Model import.** glTF to SSBH plus NUTEXB writer, reusing an existing skeleton first. Then new skeletons and animation sets.
6. **New course content.** New geometry (glTF to SSBH), lightmaps or probe bake, collision generation, course text and unlock rules.

## Risks and open questions

- **New entries need loader work.** The mod hook only replaces entries the game already requests. New archives and new IDs need the DRP index lookup decompiled and `main` regenerated.
- **Field meanings are inferred.** Most levelb fields are named from data patterns only. A writer must keep unknown fields exactly, and each edit type needs an in-game check.
- **Collision for new geometry.** `DC4A8177` and `navmshb` are decoded and edited in place (see "Ground formats"). Imported scenery adds a collision shape (item 4); new navmesh polygons are not written yet, so Pokémon do not walk around it.
- **Lightmaps on reshaped ground.** Reshaped ground keeps its baked shading; a raised hill is lit as the flat ground was.
- **Lighting.** Light probes (`lprb`) and lightmap UVs are baked. New geometry may look wrong until there is a probe bake path or a fallback.
- **Registries.** Course registration, unlock rules, scoring thresholds and research entries span several registries with partial schemas. Where scoring thresholds live is not confirmed.
- **Save identity.** How saves store course and species IDs, and what an unmodded game does with unknown IDs, is not checked.
- **Behavior scripts.** `luo` files are compiled Lua 5.1. Editing behavior needs a decompile and recompile path with matching bytecode format.
- **Unresolved hashes.** Some courseb level hashes do not match any entry name in their pack (Florio resolves fully; other packs not surveyed).
- **Repository tracking.** The root `.gitignore` ignores `tools/`, so the editor source is not tracked by git today. Tracking it needs the root rule changed from `tools/` to `tools/*` plus `!tools/level-editor/` (git cannot re-include a folder whose parent folder is ignored); the editor's own `.gitignore` already keeps `target/` and `shots/` out.
