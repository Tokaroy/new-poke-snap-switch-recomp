# Foliage and particle display upgrades

Code: `runtime/nvn/nvn_foliage.cpp` / `.h`, hooks in `nvn_draw.cpp` (pipeline description and fragment variants) and `nvn_entry.cpp` (texture creation).
Three rows, all off by default, all display only (photo frames, shadow maps and the Evaluation targets keep the game's own draw):

| Row (F1, native tab 11) | Key / env | Values | Live | What it changes |
|---|---|---|---|---|
| Leaf edge smoothing | `foliage_aa` / `SNAP_FOLIAGE_AA` | Off, On | Restart (live in the dev loop) | Alpha-tested scene draws dither their edge instead of a hard discard |
| Leaf translucency | `leaf_translucency` / `SNAP_LEAF_TRANSLUCENCY` | Off, Subtle 0.5, Strong 1 | Restart (live in the dev loop) | Back-lit glow on alpha-tested lit materials when the camera looks toward the sun |
| Soft particles | `soft_particles` / `SNAP_SOFT_PARTICLES` | Off, On | Restart | The half-resolution particle layer is drawn at the scene's resolution |

Wind sway (`SNAP_WIND_SWAY`, section 4) is in the code but has no menu row: it still darkens leaves, so it stays an environment variable for testing.

## What the game draws (Florio census, `analysis/foliage/census_florio.txt`, local only)

* Scene colour is RGBA16F (NVN format `0x29`), depth `0x34`. Foliage, grass and fence cut-outs are **opaque, alpha-tested** draws: alpha func GREATER (5), reference 0.3 to 0.5, blend off,
  depth test and write on, into the scene target (1920x1080, or the dynamic-resolution sizes). About 25 fragment shaders; all of them read the per-view buffer (set 0 binding 20) and the
  light buffer (binding 22).
* There is no MSAA anywhere in the port; anti-aliasing is SSAA (render scale), SMAA/CAS and the upscalers (DLSS, FSR, XeSS).
* Particles are drawn **blended into a 960x540 RGBA16F target** that has its own 960x540 32-bit float depth. A downsample draw (`fs 68e12aca46cd`) fills that depth from the full-resolution
  scene depth; many particle shaders then sample the full-resolution depth themselves (the soft fade the game already has); a composite draw (`fs 36595a7acf8d`) blends the layer back onto the scene.

## 1. Leaf edge smoothing (hashed alpha)

`PatchAlphaHash` (nvn_foliage.cpp) wraps the translated fragment shader like `PatchAlphaTest`: the wrapper calls the original main, then instead of `discard if a <= ref` it computes
`s = (a - ref) / (0.5 * fwidth(a) + 1/512) + 0.5` and keeps the fragment when `s` beats a per-pixel interleaved-gradient noise value of `gl_FragCoord`. The contour stays where the game puts it
(`a == ref`); the hard edge becomes a ramp about one pixel wide, which SSAA, SMAA and the temporal upscalers average into an anti-aliased edge. Flat alpha keeps the game's exact decision.
When the target is multisampled the variant instead writes `s` to the alpha output and the pipeline enables `alphaToCoverageEnable` (that branch is wired but no MSAA target exists in the port, so it is untested).

* Qualifying draws (`FoliagePipeBits`): alpha func LESS/LEQUAL/GREATER/GEQUAL, reference at least 0.15, blend off, colour RGBA16F, depth present and tested, not a photo frame. UI, shadow casters,
  blended particles, 8 bit targets and every photo/Evaluation draw keep the game's own pipeline.
* A half width ramp is used: the full width ramp left visible dark speckle inside dense canopy at 4K because nothing resolves the noise without a temporal pass. The effect is meant for use with
  the anti-aliasing / upscaler options; at native resolution with no AA it trades stair-steps for fine noise.

## 2. Leaf translucency

Same wrapper (`leaf` level 1..8 in `PipeDesc.pad2` bits 4..7, variant key bits 11..14). It reads the camera direction (`PerViewCBuffer`, set 0 binding 20, vec4 37) and the light direction (binding 22, vec4 1, the direction the light travels, negated here to get
the direction toward the light) that the game's own lit foliage shader already binds, forms `d = dot(forward, toLight)`, and, as the camera turns toward the sun (`d > -0.35`, strongest at `d = 1`), multiplies the lit colour by
`1 + 2.5 * level/8 * ((d + 0.35) / 1.35)^2 * tint` with tint (1, 0.9, 0.35): warm light shining through the leaf. Shaders without those two buffers (or without the GLSL.std450 import) get no leaf term.
It applies to every alpha-tested lit material in the scene pass (leaves, grass, cut-out props), not to Pokémon bodies (opaque, no alpha test).

## 3. Soft particles (full-resolution particle layer)

`FoliageParticleBoost` (called from `TextureInitialize`) recognises the particle targets by their creation call site (`main+0x5be358/0x5bfde8/0x5bfe08/0x5c09ac`, a 960x540 RGBA16F or D32F texture) and makes them twice as
large per side as the scene scale gives them (`t->scale *= 2`). The particle pass draws into colour **C** with the depth of **B**; B's colour is the downsample draw's own output, so B colour, B depth and C
are boosted together (A, the unrelated pair created at `0x5be358`, is left alone; `SNAP_SOFT_PARTICLES_SEL` bit 0/1/2 = A/B/C for debugging). The downsample draw then samples the full-resolution depth
one to one, so the particle depth test no longer has the half-resolution halo, and the existing soft-fade shaders compare against full-resolution depth. Particle shaders that have no depth
sampling of their own do not get a fade (that needs a new descriptor binding per shader; not done).
The targets are sized at texture creation, so the row applies after restart.

## 4. Wind sway (experimental, environment variable only)

`SNAP_WIND_SWAY=0.5|1` wraps the vertex program of the same alpha-tested scene draws (`PatchVertexSway`, `FoliageSwayModule`): the wrapper runs the original main, rebuilds the world position from `gl_Position` with the per-view buffer
(set 0 binding 4: view projection at vec4 16..19, its inverse at 20..23, absolute timer at vec4 42 `.w`), adds a horizontal wind displacement (slow gusts travelling downwind plus flutter) as `viewProjection * (dx, 0, dz, 0)`, and writes it back.
Only `gl_Position` changes. The amplitude (world units, default 0.05 at full strength, `SNAP_WIND_AMP` overrides) and a time offset (`SNAP_WIND_PHASE` 0..7, for frozen frames) are specialization constants of the patched module. `SNAP_WIND_STATIC=1` freezes the field.
`PipeDesc.pad2` bits 17..20 carry the level, bits 21..23 the phase offset. Photo frames keep the game's vertex shader.

What the capture work established (`analysis/foliage/ab_sway_*.png`, local only):

* The colour pass tests depth with GEQUAL against a colour-less pre-pass that writes depth. Swaying only the colour draw makes the leaves fail their own depth test (hatched black leaves even at a 1e-5 amplitude), so the pre-pass draws (no colour target, GEQUAL, depth write) get the same wrapper. Shadow maps (LESS) keep the game's shader. This fixed the hatching.
* With a 1e-7 amplitude the swayed frame equals the Off frame (`ab_sway_tiny.png`), so the wrapper itself is neutral.
* At 0.025 to 0.3 world units many leaves and ground plants still render dark or black (`ab_sway_default.png`, `ab_sway_fix3.png`), and the darkening does not scale with the amplitude. A static field (`ab_sway_static.png`) and also swaying the alpha-tested shadow casters (`ab_sway_shadows.png`) did not remove it. The cause is not found: something that lights the leaves still sees their original position.
* Until that is fixed there is no menu row, no test row and no preset entry.

## Photo safety

* `FoliagePipeBits` returns 0 on every `FxIsPhotoFrame()` present, so photo/Evaluation/360x240 draws use the original pipelines (identical `PipeDesc`, identical shader module).
* The particle targets are only used by the scene pass; the Evaluation and capture targets are exempt as before.
* The A/B tool's gating counters (`FxNoteRan` is called only when a variant is chosen) count variant draws on photo presents; the expected value is 0.

## Proof and cost

All files are in `analysis/foliage/` (local only, ignored by git). Strips are Off on the left, On on the right, one frozen frame per value from one `ab_capture.ps1` run (`-Route core_florio_a -Moment ride8`), top row the full frame, bottom row a magnified crop.

| Effect | Strip | Changed pixels |
|---|---|---|
| Leaf edge smoothing | `ab_aa_ride8.png` | 2.3%, edge pixels only; the animated portrait alone changes 2.4% in a control pair (5.9% with the first full width ramp) |
| Leaf translucency (Strong) | `ab_leaf_ride8.png` | 18.6% |
| Soft particles | `particles_off_on_ride20.png` (two runs; the targets are sized at creation) | not comparable across runs |

Cost at 3840x2160 (`SNAP_RENDER_SCALE=2`), one exclusive `ab_capture -Exclusive` run on a frozen Florio frame, mean GPU ms per frame over 60 presents (`ab_bench4k.png` / `.json`; the dev loop's named passes, so it excludes the CPU and the present):

| State | GPU ms per frame |
|---|---|
| Off, control run at the end | 1.556 |
| Leaf edge smoothing | 1.580 |
| Leaf translucency (Strong) | 1.576 |
| Wind sway (full) | 1.539 |
| All three | 1.524 |

All four are within 0.03 ms of the control, which is the run to run noise of this measurement; none of them has a cost measurable on this frame. The first value in the run (Off) read 4.63 ms: shader and pipeline warm-up, not a cost, so it is discarded. The earlier shared-slot figures (+0.4 ms and +0.6 ms) were noise from other runs sharing the GPU. The frame is one Florio view; a denser canopy would cost more. Soft particles are restart only and were not part of this run: their layer fill is 4x (the particle passes read 0.05 ms at the scene scale and about 0.2 ms at 4x the pixels in the shared-slot dev loop).

Notes on the evidence:

* `ab_leafdbg2_ride8.png` is the debug paint (`SNAP_LEAF_DEBUG=1`, `SNAP_LEAF_FLIP` 0/1). It showed that the light buffer holds the direction the light travels, so the first sign was wrong. At `ride8` the sun is off to one side and the term needs the wider lobe now in use; at `ride20` the camera looks into shade and the term changes no pixel (`ab_leaf_ride20.png`).
* The soft particle strip has no visible dust, pollen or spray in either state. Tried: `core_florio_a` at `ride8` and `ride20`, `core_volcano_0` at `ride30` and `ride70` (a snow scene, `ab_ab_volc*_{0,1}.png`), and `core_beach_0` at `beach_sea` (`ab_volc_beach_{0,1}.png`); two runs each with `SNAP_SOFT_PARTICLES` 0 and 1 at `SNAP_RENDER_SCALE=2`. The pass sizes in `ab_particles*_ride*.json` show the layer moving from 1920x1080 to 3840x2160 and no visual corruption appeared. A moment with a particle burst in frame is still missing, so the soft fade itself has no before/after picture.
* Photo safety: `ab_capture` reads PHOTO-FRAME CHECK INCONCLUSIVE at `ride8` and `ride20` (shutter not accepted at a frozen moment), so the gating counters did not run. Instead the build logs `[foliage]` lines: a full `florio_eval` ride with all three rows on (`SNAP_FOLIAGE_AA=1 SNAP_LEAF_TRANSLUCENCY=1 SNAP_SOFT_PARTICLES=1`, run `nl-20261116_012343`) logs the rows as on, reaches `evaluation:Score` and `End`, and logs no photo frame draw that took a variant (`score_allon_florio_eval.png`: Dodrio, total 2,467, inside the 2,337 to 2,658 spread of unmodified runs; the photo differs run to run, so a total cannot isolate an effect). `FoliagePipeBits` returns 0 when `FxIsPhotoFrame()` is true, so a photo draw gets the identical pipeline and shader module.
