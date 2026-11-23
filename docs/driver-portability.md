# Driver portability: running on a software Vulkan driver (2026-11-23)

Goal: find code that only works because the development GPU (RTX 4090, NVIDIA driver 621.24) is lenient: missing barriers, formats, extensions assumed present, NVIDIA-only behavior. Method: run the game on Mesa lavapipe (llvmpipe, a CPU Vulkan 1.4 driver) with the Khronos validation layer and synchronization validation on, and compare with the same run on the NVIDIA driver.

## How to run

1. Get a Mesa build with lavapipe. Used here: mesa-dist-win release 26.3.1 (`mesa3d-26.3.1-release-msvc.7z`, from the pal1000/mesa-dist-win GitHub releases, MIT-licensed Mesa), SHA-256 `64e84cf13d67dd354640bf443ed0445e6a83afd06a5729f0750b3744d633b3f1` (equal to the digest GitHub shows for the asset). Unpack it into a folder outside the repo; the driver is `x64\vulkan_lvp.dll` with `x64\lvp_icd.x86_64.json`. Mesa binaries are never committed.
2. `powershell -File host\run_lavapipe.ps1 -MesaDir <...>\x64 -BuildDir <build dir> -Route lvp_menu` (title to the lab hub menu) or `-Route lvp_ride` (to the first seconds of the Florio ride). It selects lavapipe for that process only with `VK_ICD_FILENAMES` and `VK_DRIVER_FILES`, turns on `NVN_VK_VALIDATION=1` (sync validation comes with it) and `VK_LAYER_PATH=tools\vulkan_sdk\Bin`, and runs `host\run_boot.ps1` hidden. Nothing is installed system-wide and the registry is not touched. `-MesaDir ''` runs the identical settings on the normal GPU.
3. Defaults to a small render (`SNAP_AA=off`, `SNAP_RENDER_SCALE=0.25`, 640x360 window). `-Aa default` keeps the game's default (ssaa2 plus SMAA, 1080-line render).
4. Seed the shader cache first (copy a warm `nvn_cache` into the build dir): the translated-shader cache is SPIR-V and driver independent. The Vulkan pipeline cache is keyed to the driver, and a blob from another GPU is ignored by the loader, so no cache mixing problem arose.

Cost on this desktop CPU: title in 40 s (present 750), lab hub menu at 80 s, map select at 180-360 s, ride start at 260-450 s, about 1 present per second in the ride at 0.25 scale. The default 1080-line render reached the menu in 357 s. Every run stayed under 10 minutes.

## What the runtime needs vs what lavapipe offers

| Item | NVIDIA 4090 | lavapipe (llvmpipe, LLVM 23.1.6, 256-bit) | Behavior in the runtime |
|---|---|---|---|
| API version | 1.4 | 1.4.363 | Vulkan 1.3 is required: met |
| Core 1.0 features (robustBufferAccess, shaderFloat64/Int64, geometryShader, dualSrcBlend, BC textures) | yes | yes | all enabled as offered |
| 1.1 to 1.3 required (synchronization2, dynamicRendering, timelineSemaphore, shaderDrawParameters) | yes | yes | met |
| samplerMirrorClampToEdge, hostQueryReset, shaderDemoteToHelperInvocation | yes | yes | enabled when offered |
| VK_EXT_provoking_vertex, VK_EXT_custom_border_color | yes | yes | both enabled (`provokingVertexLast=1 customBorderColor=1`) |
| subgroup size control, pin to 32 lanes | min 32 max 32, pin works | subgroup size 8 only, 32 not in range | pin off (`subgroupPin=0x0`), see finding 1 |
| VK_KHR_present_id, VK_KHR_present_wait | yes | absent | `[pace] present wait not offered`, falls back to the model display pacing; clean |
| VK_EXT_graphics_pipeline_library | yes | yes | off by the quirk table (`gpl_unmeasured`); `SNAP_GPL=on` also run, see below |
| VK_EXT_hdr_metadata, swapchain color space | yes | yes | not exercised (SDR runs) |
| Win32 presentation, swapchain | yes | yes | works; vendor id 0x10005 is read correctly by the quirk table |

## Findings

| # | Run (hidden, validation + sync validation) | NVIDIA | lavapipe |
|---|---|---|---|
| 1 | `lvp_menu`, 0.25 scale | reaches title and lab hub menu, 0 messages | same, 0 messages |
| 2 | `lvp_ride` (Florio start), 0.25 scale | rides, 0 messages | rides, 0 messages (ride reached at 261 s and 447 s in two runs, route ended by its own `end`; a 170 s ride exceeded the 585 s cap, exit 4, no crash) |
| 3 | `lvp_menu`, game default AA (ssaa2 + SMAA + CAS, 1080 lines) | 0 messages | 0 messages, menu at 335 s |
| 4 | `lvp_menu` with `SNAP_GPL=on` | 0 messages | 0 messages |
| 5 | VUIDs | none | none |

Validation errors by VUID: 0 on both drivers in every run (the layer was confirmed active: with `VK_LAYER_PATH` pointing at an empty folder the runtime logs `validation requested but VK_LAYER_KHRONOS_validation not found`). No crashes or hangs on lavapipe.

Visual comparison (half-size frame dumps in `analysis\boot\`, not committed): the title card, the lab hub scene with the HUD icons and the Florio ride (path, trees, HUD, shot counter, mini-map) look the same on both drivers at 0.25 scale and at the default 1080-line render. The runs reach the same scene at different presents (lavapipe is about 1 fps, so the game logic runs fewer presents per scene), so frames are matched by scene, not by present number; no per-pixel diff was made.

1. **32-lane subgroup assumption (open, one log added).** `EvaluationAggregatePS` (photo scoring) and `nuFeLightCullingCS` are translated from Maxwell and assume 32 lanes. The runtime pins them with `requiredSubgroupSize = 32` when the GPU allows it. lavapipe has 8 lanes only, so the pin is silently unavailable and the lane math runs at 8: scores or light culling may differ. Intel GPUs typically report 8 to 32, so a GPU without 32 in its range is plausible in the field. Fixed so far: a one-time `[nvn] warning: ... cannot be pinned to 32 ...` line when such a module is built unpinned (`nvn_draw.cpp` `PinSubgroup32`; tested with `SNAP_VK_SGPIN=0` on the NVIDIA GPU, the warning appears once during the ride start). Not fixed: a real 8-lane scoring path. Could not be measured: a photo-scoring ride on lavapipe (the Florio evaluation run needs about 17,000 presents, hours at 1 fps).
2. **GPL stays off everywhere.** `gpl_unmeasured` turns GPL off on every GPU. lavapipe built the GPL path without validation messages with `SNAP_GPL=on` (menu route), which is one more data point for that table, not a speed measurement.
3. **No present_wait on software drivers.** Handled (the fallback is logged once).

## Fixes (branch `lavapipe`)

- `runtime/nvn/nvn_draw.cpp`: one-time warning when a subgroup-dependent shader cannot be pinned to 32 lanes (log only, no behavior change).
- `host/run_lavapipe.ps1`, routes `host/routes/lvp_menu.txt` and `host/routes/lvp_ride.txt`: the way to repeat these runs.

Checks after the change: HLE suite 2811 checks, 1 failed (`padfx`, known), `tests\mods\run_tests.ps1` passes, a warm-cache NVIDIA `florio_eval` ride reaches `ui=evaluation:End` and `end of route`.

## Open issues

- Photo scoring and light culling at a native subgroup size other than 32 are untested. A way to test on this machine: none without a ride-length lavapipe run. Candidate work: derive the lane count from the pipeline, or fall back to a subgroup-size-independent variant of the two programs.
- A pixel-level image comparison at an identical game state (needs a frame-exact state hook) would make the visual check stronger; lavapipe is not bit-exact with a hardware GPU anyway (rounding, derivatives), so a tolerance would be needed.
- Longer lavapipe rides (more shaders, shadows, water, particles, the HD effects passes) were not run; the sweep to repeat on a slow driver is Florio plus Beach, with `SNAP_VISUALS=enhanced`.
