# Ultrawide (21:9, 32:9)

On a window wider than 16:9, the 3D view gets wider: the vertical field of view stays the same and the sides show more of the world. The interface, menus, videos and photos keep the original 16:9 layout in the middle.

## Setting

| Where | Values | Default |
|---|---|---|
| `SNAP_ULTRAWIDE` | `auto`, `off` | `auto` |
| Launcher and F1 > Graphics: "Ultrawide" (`gfx_ultrawide`) | Auto (wider 3D view), Off (16:9 with side bars) | Auto |

The setting needs a restart. The factor is k = window aspect / (16/9), capped at 4. It is read once from the window's client size when the game starts (`host/boot.cpp`, log line `ultrawide: auto, window client WxH, factor k`). A window at 16:9 or narrower gets k = 1, and every path then runs exactly as before. Off keeps the 16:9 picture with black bars.

## How it works

Code: `runtime/nvn/nvn_wide.cpp` (the draw rules), `host/mods/lodanim.cpp` (`wide_camera`, the camera), plus small hooks in `nvn_draw.cpp`, `nvn_entry.cpp`, `nvn_aa.cpp` and `nvn_fx.h`, each marked `[wide]`.

1. **Camera.** The ride and lab camera builder (`main+0x295170`) loads the aspect into `s9` just before its `tanf` call at `main+0x296400`. The `tanf` hook multiplies that register by k when it holds 16/9, so the projection keeps its vertical field of view and gets k times the width. The CPU camera is widened too, so culling and level of detail cover the new edges and nothing pops in at the sides. Other 16:9 cameras (cutscenes, some menus) are not widened.
2. **Render targets.** Every 16:9 render target (the scene, its depth, the post chain, the window textures) gets an image k times wider (`TextureImpl::wide`; `hostW = ceil(width * scale * k)`). The Evaluation depth target and the photo targets keep their size.
3. **Per draw.** Each draw into a wide target picks how its guest x maps onto the image (`ReplayCtx::wideMode`):
   - **full** (x times k): draws with the widened camera, post passes that sample a wide image, full-screen fills, and full-screen or screen-quarter overlays while the 3D view is wide (dimming, wipes, the viewfinder frame).
   - **narrow** (the middle 16:9): interface draws and 16:9 cameras that were not widened.
   - **effects**: world-space particles drawn with a 16:9 camera through the shared camera buffer (c3) get a widened copy of that buffer, so they line up with the widened world. Set `SNAP_WIDE_NO_C3=1` to keep them narrow.
4. **Photos and scoring.** A draw with the widened camera into a 16:9-only target (the photo Evaluation passes) uses mode **unwiden**: the viewport is k times wider around its center, which gives back the original 16:9 projection exactly. A 16:9 target that samples a wide image reads its middle 16:9 at full resolution (`WideCropSubstitute`). Saved photos keep the original framing.
5. **Present.** A frame with a widened 3D draw is shown at full width. A frame with none (menus, photo select, movies, cutscenes) shows the middle 16:9 with black bars (`WidePresentCrop`). When the game sets its own present crop (it does on some loading and menu screens), that crop is mapped into the middle 16:9 too. The decision holds for 4 presents so the bars do not flicker on screens that draw their 3D backdrop every other frame.

## What to expect

- The ride, the lab hub and other widened 3D scenes fill the screen. The HUD stays 16:9 in the middle.
- The rounded viewfinder frame the game darkens around the view during camera moments is stretched with the view (the original draws it at about 12% to 88% of the width and height, and it keeps those fractions).
- Menus, photo select, the evaluation screens and videos show 16:9 with side bars.

## Known limits

- **Photo scoring on the CPU side.** The projection stored with each photo at the shutter is the widened one (a 3:2 photo camera at aspect 1.5k; seen in the camera heap as `m11 / m00 = 3.0` at k = 2). The GPU Evaluation passes unwiden it exactly. Any score part the game computes on the CPU from that stored projection (possibly Placement or Other Pokémon) would see a frame k times wider. That code was not found, and runs are not repeatable enough to compare totals, so this is unverified. If scores look off, compare with `SNAP_ULTRAWIDE=off`.
- Post effects that use a horizontal aspect constant (the edge focus blur, vignette and lens-distortion tonemap) keep the 16:9 constant over the wider image, so their shape is stretched by k.

## Developer switches

- `SNAP_WIDE_LOG=1`: one summary line per present change; `=2`: each distinct draw decision once (target, rule, camera kind, box, sampled sizes, fragment shader key); `=3`: widened-camera draws too. `SNAP_WIDE_LOG_FROM=<present>` starts the log later.
- `SNAP_WIDE_TRACE=1`: logs each caller that builds a camera at a widened site.
- `SNAP_WIDE_SITES=0xOFF:reg,...`: extra camera `tanf` sites to widen (main offset and the float register that holds the aspect).
- `SNAP_WIDE_NO_C3=1`: 16:9 effect cameras stay narrow.

## Tests

Florio, route `score_time_a`, at 5120x1440 (k = 2) and 2560x1080 (k = 1.3333). No Vulkan validation errors in any run.
