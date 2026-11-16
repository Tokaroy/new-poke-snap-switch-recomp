# HUD scale

The interface (ride HUD, menus, dialogue boxes, the viewfinder) is drawn for a 1920x1080 screen. On a 1440p or 4K window it looks small. The HUD scale makes the interface bigger. The 3D view, photos and videos keep their size.

## Setting

| Where | Values | Default |
|---|---|---|
| `SNAP_HUD_SCALE` | `auto`, `100`, `125`, `150`, `175`, `200` | `auto` |
| Settings key `hud_scale` | the same values | `auto` (key removed) |
| Launcher > Display: "HUD scale" | Auto, 100% to 200% | Auto |
| F1 > Display: "HUD scale" | Auto, 100% to 200% | Auto |
| Game Settings > native PC tab "Interface": "HUD scale" | Auto, 100% to 200% | Auto |

The setting is live: the next frame uses it.

Auto is 100%, the original look, at every window size (2026-11-16 hotfix). Auto used to pick 125% at 1440p, 150% at 4K and more above that, but on the owner's 4K screen panels sat off-center and oversized and typewriter dialogue clipped; a size is only used when you choose it.

## How it works

Code: `runtime/nvn/nvn_hud.cpp`, with small hooks in `nvn_draw.cpp` and `nvn_vk.cpp` (marked `[hud]`) and in `runtime/hle/input_core.cpp` (touch).

1. **Which draws.** The game draws its interface straight into the 1920x1080 window texture as many small sprite and text draws, each with a box in guest pixels. A draw is scaled only when it goes into that texture, uses no depth buffer, has a box that lies on the screen and is smaller than 90% of the screen in width or height. Backdrops, fades, wipes, full-screen layers, the 3D scene, post passes and photo targets are never scaled.
2. **How.** The viewport and scissor of the draw are changed before it is emitted: x' = p x + q, y' = r y + t, with p = r = the scale. Vertex data and shaders are not touched.
3. **Clusters.** Scaling each draw on its own would pull a button apart from its label. Boxes that overlap or lie within 40 guest pixels of each other form a cluster (a button with its icon and label, a film counter with its frame). Clusters are built from one frame's draws and used by the next frame (the interface hardly changes between two presents).
4. **Where it goes.** A cluster scales about the screen center, by at most the factor that still lets it fit the screen with a 3% margin (a panel that already fills the screen stays at 100%). If the scaled box would cross a screen edge, it moves back inside, keeping at most its original distance from the edge. Corner icons stay in their corners.
5. **Ultrawide.** The transform is applied to the guest viewport first, then the ultrawide mapping (`nvn_wide.cpp`, mode "narrow") places it in the middle 16:9, so the HUD scales inside the centered 16:9 area.
6. **Touch and mouse.** The mouse (click-to-touch) maps the window position to guest pixels through the last present's blit rectangle, then undoes the cluster transform that drew that frame. A click lands on the element it was drawn on at every scale, on ultrawide windows and with bars.
7. **Photos.** Photos render into non-display targets and are not affected. The Evaluation and saved photos have no HUD, as before.

## Known limits

- A cluster is one frame behind its draws. The first frame of a new screen draws at the cluster of the previous frame, or the draw alone if none matched.
- Compound widgets that are more than 40 guest pixels apart (a far-apart label) scale as separate clusters.
- A menu whose clusters merge into one block that fills the screen stays near 100%, because it already uses the whole screen.
- The host's own overlays (the F1 menu, the launcher) are not part of the game's interface and are not scaled by this setting.
- Hidden test runs report a 3840x2160 window whatever `SNAP_WINDOW_H` says when the saved `window.json` has `"mode": "borderless"` (the window then fills the hidden desktop). Add `SNAP_WINDOW_MODE=windowed` to get the requested size: with `SNAP_WINDOW_W=2560 SNAP_WINDOW_H=1440` the log shows `window client 2560x1440` and `[hud] window height 1440: scale 1.25 (auto)`.
- The `SNAP_HUD_LOG=3` touch self-check can report a miss at 125% to 150% (for example 12 of 13). The lookup picks the smallest shown box under the point, so a box drawn on top of another one's center wins; a miss is expected for stacked draws and was not traced to a real touch error.

## Developer switches

- `SNAP_HUD_LOG=1`: one line when the window height changes the scale. `=2`: also the clusters every 120 presents. `=3`: also a touch self-check every 240 presents (how many shown boxes map back to their own center).

## Results

Route `intro_probe` (Florio ride, hidden, 3840x2160 window), screenshots in `analysis/optpair_proof/` (local, not committed):

| File | Scale | Run |
|---|---|---|
| `hud_100pct.png` | 100% (original look, `SNAP_HUD_SCALE=100`) | `1-20261116_005403` |
| `hud_125pct.png` | 125% (`SNAP_HUD_SCALE=125`) | `0-20261116_041419` |
| `hud_150pct_4k_auto.png` | 150% (Auto at 4K) | `1-20261116_034441` |
| `hud_200pct.png` | 200% (`SNAP_HUD_SCALE=200`) | `0-20261116_042130` |
| `hud_scale_compare.png` | the four side by side | |

- The ride HUD (film counter with its number, corner icons, the viewfinder frame, the professor's box and speech bubble) grows around the screen center, and the corner icons stay in their corners. The 3D view is the same size in all four. The film icon box measures 126, 156, 188 and 254 px wide at 100%, 125%, 150% and 200%.
- Text draws place their glyph quads in pixels local to the text and take the text origin from the vertex uniforms. The hook adds that origin back before it builds the cluster, so the film counter digits scale with their icon.
- Touch self-check (`SNAP_HUD_LOG=3`): at 150%, 4 of 5, 13 of 13 and 20 of 20 shown boxes map back to their own center; at 200%, 4 of 5, 12 of 12 and 20 of 20. The one box that misses in the first check was not examined.
- Photos: the Score-screen photos of the skip-intro score runs (HUD scale Auto at 4K, 150%) look the same as at 100%. No separate pixel comparison of a photo with the scale on and off was made.
- Not tested: a real 1440p window (hidden runs report 4K, so 125% was forced with `SNAP_HUD_SCALE=125`), and the ultrawide mode together with the HUD scale.
