# HD AI textures

The builder can make sharper textures for your game on your own PC. It saves the textures your copy of the game loads, upscales them with a local AI model on your graphics card, and installs the result as a mod pack called `hd-ai-textures`. Nothing is uploaded, and the release contains no textures, no AI models and no game data. Every download comes from its author, after you accept its license.

## Quick start

```
python builder/snap_builder.py hdtex --collect <out folder>\SnapRecomp.exe
python builder/snap_builder.py hdtex --level hd
```

The first command runs the game hidden and muted through each course your save has unlocked and saves the textures it loads. The second builds the pack and turns it on. Start the game as usual.

You can also skip `--collect`: run `hdtex --capture on`, play normally for a while, then build. Textures load the first time the game shows them, so a course you have not visited yet stays at its original resolution until you capture it and build again. A rebuild only does the new textures.

## Levels

| Level | Courses | Pokémon | Menus | Lighting | Notes |
|---|---|---|---|---|---|
| Off | | | | | turns the pack off; deletes nothing |
| Sharp | 2x | original | 2x | original | quickest |
| HD (suggested) | 4x | 2x | 2x | 2x | |
| Ultra | 4x, up to 4096 | 4x | 4x | 4x | normal maps and masks 2x; crisper grain on tiling ground and rock |
| Custom | your choice | your choice | your choice | your choice | `--level custom --set creature.scale=4 --set scenery.model=texture-dat2` |

Custom keys per category (`scenery`, `creature`, `ui`): `scale` (0, 2 or 4), `model`, `strength` (0 = plain resize, 1 = all AI) and `denoise` (0 to 1, runs a cleanup model first). `lighting` takes `scale`, `model` and `strength`: `--set lighting.scale=0` turns the Lighting row off, `4` doubles it again. `normal`, `mask`, `emissive` and `effect` take `scale` only.

Before it starts, the builder prints how many textures it will make, how long it expects to take, the disk space, and the video memory the busiest course will need. It stops if that does not fit your card (`--force` builds anyway). `--dry-run` prints the plan and stops.

On the test PC (RTX 4090), one course (Florio, about 6,000 captured textures) gave these numbers. Time grows with each course you capture.

| Level | Textures | Time | Disk | Video memory, busiest course |
|---|---|---|---|---|
| Sharp | 3,163 | about 23 min (estimate) | 2.8 GB | 2.6 GB |
| HD | 3,856 | 114 min (measured, with other work on the same card), plus about 4 min of light maps | 4.1 GB | 3.6 GB |
| Ultra | 4,924 | about 185 min (estimate) | 13.5 GB | 12.0 GB |

The HD build was timed while other programs were using the graphics card, so it took about twice the 55 minutes `--dry-run` printed. Before it starts, the builder times each model on your card and prints a corrected estimate. Ultra takes about three hours per course because Pokémon and menus go to 4x with the slower, finer model. A rebuild only makes what changed, and the estimate counts only those textures.

## What each kind of texture gets

- **Course surfaces** use `nomos2-plksr` at full strength. Textures that tile are padded with their own opposite edge, so seams stay hidden.
- **Pokémon** use `texture-dat2` at half strength, mixed with a plain resize, so fur stays fine and soft instead of turning thick.
- **Menus and icons** use a small model trained on flat art, which keeps text and lines clean.
- **Normal maps** are resized as directions and set back to unit length, not run through the AI.
- **Masks, shadow maps, glow maps and effects** are resized with a sharp filter and never get AI detail.
- **Light maps** (the Lighting row) are the light and shadow baked into the courses. See the next section.
- Mip maps are made in linear light. Cut-out alpha, such as leaves and fences, keeps the same coverage at every mip level so foliage does not thin out in the distance.
- BC3 color maps are rewritten as BC7 (same size, better quality). `--bc7 all` also converts BC1 (twice the memory); `--bc7 off` keeps every format.

## Lighting

Each course stores its baked light in sets of four BC6H textures per mesh, named `...^0^Lightmap` to `...^3^Lightmap`. Only the `^0^` map is light. The other three are signed directional terms (values from about -32 to 58) and are never changed; changing them would bend the light's direction.

The `^0^` maps go through these steps:

1. Texels outside the UV islands (about a fifth of each map) are filled from the nearest island (push-pull), so neither the resize nor the game's own filtering pulls black into an island's border. The filled values stay in the output instead of being set back to black, which is what stops dark seams along UV borders.
2. The light goes into log2 space and is spread over the map's own range, as 32-bit floats. Nothing is rounded to 8 bits, so smooth gradients cannot band.
3. `aomr2` makes the 2x version (two passes for 4x). On the 12 light maps of the bench it rang least and added the least noise in flat light of the models tried (`texture-dat2` drew a cell pattern in noisy areas, `spanimation2` erased small shadows, `nomos2-plksr` rang three times as much as a plain resize).
4. The model's output is used only along clear shadow edges. In flat light and where the map is noisy, a plain log-space Lanczos resize is kept. On the bench this cut ringing by 40% against Lanczos alone, with edges as sharp as the model's.
5. Back in linear light, the result is matched to the plain resize at low frequencies (factor 0.6 to 1.6), so the total light, its color and its spread stay the game's. In the unit test the light inside the islands stays within 2%.
6. texconv (DirectXTex, MIT) writes it back as BC6H in the original's format (signed or unsigned).

Checked on the 433 light maps of Florio and the beach:

- Seams: along UV island borders, the game's bilinear filter now sees 1.01 times the island's own light. The originals see 0.58 (a dark seam), and setting the gutter back to black gives 0.54.
- Energy: the light inside the islands stays within 0.7% of the original for 90% of the maps (mean 0.9994).
- Banding: a smooth gradient keeps a median of 151 distinct levels per 16x16 block, against 21 when the log light is rounded to 8 bits first.
- In game, at the same camera on Florio (day) and the beach, the whole screen kept 99.5% to 100.1% of the original's light, with no seams along rock and ground edges.
- Photo scores on a Florio day ride stayed within the spread between two runs of the original game.

The replacements load by path through the same mod pack as the other textures, and F8 compares them with the originals in game.

## Models

| Model | Used for | Speed (4090) | License |
|---|---|---|---|
| `nomos2-plksr` (default) | courses | fast | CC BY 4.0, Philip Hofmann |
| `texture-dat2` (default) | Pokémon: fine, soft fur | about 10x slower | CC BY 4.0, Philip Hofmann |
| `spanimation2` (default) | menus | fastest | MIT, TNTwise |
| `nomos2` | older, faster course model; makes blocky patches on compressed textures | fast | CC BY 4.0, Philip Hofmann |
| `gtav-rgts` | realistic surfaces | about 10x slower | CC BY 4.0, Philip Hofmann |
| `aomr2` (default) | light maps; 2x, faithful | fast | CC BY 4.0, Philip Hofmann |
| `dejpg` | cleanup pass (`denoise`) | fast | CC BY 4.0, Philip Hofmann |
| `x4plus`, `x4plus-anime` | Real-ESRGAN, any Vulkan card | slower | BSD-3-Clause, Xintao Wang |
| `ultrasharp2` | sharp, general | about 9x slower | personal, non-commercial use only |

The AI runs through ONNX Runtime with DirectML, which works on any DirectX 12 card (NVIDIA, AMD or Intel). The Real-ESRGAN models run through `realesrgan-ncnn-vulkan`. Each model is pinned by SHA-256 in `builder/hdtex_pins.json`; a file that does not match is deleted.

Licenses are asked once and recorded in `tools\hdtex\licenses.json`. To accept them up front, pass `--accept-license directml --accept-license texconv --accept-license model:nomos2-plksr --accept-license model:texture-dat2 --accept-license model:spanimation2 --accept-license model:aomr2`.

## Turning it on and off

- `hdtex --level off` turns the pack off. `hdtex --level hd` turns it back on and rebuilds only what changed.
- Changing level keeps the textures the new level does not need in the work folder, so switching back is quick.
- `--exclude 25,133` leaves those Pokémon (National Pokédex numbers) at their original textures.
- A pack you made yourself and listed earlier in `mods.json` still wins over this one.
- `hdtex --status` shows the cache, the pack and whether it is on.

## Where files go

| What | Where |
|---|---|
| Captured textures | `%LOCALAPPDATA%\SnapRecomp\texcache` |
| Work folder | `%LOCALAPPDATA%\SnapRecomp\hdtex` |
| The pack | `%APPDATA%\SnapRecomp\mods\hd-ai-textures` |
| Tools, Python and models | `%LOCALAPPDATA%\SnapRecomp\tools\hdtex` and `tools\hdtex-python` |

These folders are made from your own game files. Do not share them.
