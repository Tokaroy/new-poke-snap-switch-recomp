# Boot and load time against the other PC recomp (2026-11-23)

Numbers and method only. The other project's release (MatyMeatBoy/NewSnapRecomp v0.9.35, MIT, asset
`NewSnapRecomp-0.9.35-windows-x64-962ed263.zip`, 310,129,923 bytes, SHA-256
`06000c250d1e9eb5683319a4d05212da25c15647fa74085e80b540e447ecab34`, the value on its release page) was downloaded from its official
GitHub release page, built from the owner's own local dump with its own launcher, and measured as a black box. None of its code was read or reused.
Ours is `origin/main` at `0d3313e` (branch `boottime`, route `host/routes/boottime.txt`).

## Result

Seconds from process start, median (range) of 3 runs. "Present 1" is the first frame handed to the swapchain (black). Title, map and
ride are the first moment the screen shows them.

| Marker | Ours cold | Ours warm | Rival cold | Rival warm |
|---|---|---|---|---|
| Present 1 (first frame) | 1.38 (1.32-1.40) | 1.39 (1.33-1.40) | 2.79 (2.63-2.93) | 2.13 (2.10-2.38) |
| Title screen | 6.66 (5.97-6.77) | 6.42 (6.13-6.58) | 8.52 (8.36-8.59) | 6.91 (6.80-7.86) |
| Course map (Lental Map) | 83.8 (83.5-84.0) | 83.6 (83.4-85.9) | 79.9 (79.7-80.0) | 79.0 (79.0-79.3) |
| Florio ride started | 100.9 (100.6-100.9) | 101.0 (100.7-102.9) | 99.9 (99.9-100.2) | 99.2 (99.1-99.5) |
| Map to ride | 17.1 (17.0-17.1) | 17.0 (17.0-17.6) | 20.2 (19.9-20.2) | 20.2 (20.1-20.2) |

First-ever launch (nothing cached, one run each, same PC and save):

| Marker | Ours | Rival |
|---|---|---|
| Present 1 | 3.0 | 1.95 |
| Title screen | 357.8 (1,600+ shaders translated before the title) | 6.73 |
| Course map / ride | not measured (route ends at the title) | 79.0 / 99.4 |

Presents at each marker agree (title 250-292 ours, 260-340 theirs; map 4,718-4,768 ours, about 4,600 theirs; ride 5,702-5,745 ours,
about 5,760 theirs), so the game's own timed sequences are the same length in both and most of the 100 s is the game's lab scene and
course intro at 60 fps. The comparison that matters is wall time minus presents/60.

Launcher cost on the rival side (launcher start to game process start, 3 runs): 1.63, 0.45, 0.58 s; its Play step prepares
the "observed" shader variants first (72 variants, 18 ms in these runs).

## Where we are slower

1. First-ever launch: 357.8 s to the title against 6.7 s. Ours creates every shader program at boot (about 6,200 programs, about 1,650
   distinct shaders) and translates each one inline when the guest calls `nvnProgramSetShaders`, one after another on the loader
   threads: the log counted 1,600+ shaders translated ("first-run shader translation" lines) at about 3.8 shaders/s on a loaded PC.
   `builder/README.md` says the first launch takes "several minutes". The rival translates only what a draw needs, on 2 background
   workers, and skips or fast-links the draw meanwhile (its log: `gfx_shader: graphics shaders are translated by 2 background workers`,
   `draws skipped while their pipeline builds`); its first-ever run showed 82 frames over 25 ms in the first minute, worst 532 ms.
2. Scene transitions: three single-frame stalls of 0.78-0.90 s at presents about 1,150 (lab load), 2,890 and 4,650 (map), in all
   6 steady-state runs, about 2.5 s of the 100 s. The per-frame csv (`SNAP_PERF_SHARED`) shows the host cost of those frames near zero
   (pace, replay, pipeline, upload under 15 ms) and the guest main thread blocked on one condition variable (`0x71012fe180`) while one
   `JobWorker` burns 0.8-1.1 s of CPU in lifted game code. The rival's runs have 0.0-0.2 s excess in those windows (excess = wall time
   of a present interval minus its presents/60). The cause is not proven: the sampler (`SNAP_SAMPLE`, 500-present buckets) is too coarse to
   isolate it.
3. Map to ride: 17.0 s ours against 20.2 s theirs, but the two are not the same measurement (see below); treat it as equal.

Where we are not slower: first frame (1.4 s against 2.1-2.9 s: the rival's guest starts 0.4-1.0 s late, waiting for its pipeline
prewarm, and copies the 1.58 GB save working copy at start) and the title in steady state (6.4-6.7 s against 6.9-8.5 s). Cold and warm
barely differ for us because test runs always start with the translated-shader cache present.

## Ranked fixes

1. Translate shaders lazily, off the loader threads (first-ever launch: minutes to seconds). Boot needs the programs to exist, not
   their SPIR-V; build the program object on creation, translate on a worker pool at first draw (or when a pipeline is requested), and
   use the existing skipped-draw path while it finishes. Alternative with less risk: run the translation as a visible, parallel
   step of the builder or launcher (the harness already runs as a separate program).
2. Find the 0.8 s `JobWorker` job at the three transitions with a finer sampler bucket (`SNAP_SAMPLE`, presents/500 is too wide),
   on a quiet PC. About 2.5 s per ride start.
3. Nothing to do for first-frame or steady-state title time.

## Method

- Same PC, one game at a time, BelowNormal, hidden desktop (no window on the owner's screen), no desktop screenshots. Other chats' test
  runs (1-3 `snap_boot` processes, CPU 60-90%) shared the PC throughout; it was not quiet, so absolute seconds are upper bounds. Runs
  alternated within the same hour.
- Same save: our `unlock_all` fixture, copied into the rival's profile folder before every run (restored from a pristine copy each time).
- Rival: its executable started directly (`newsnap.exe --profile <dir>`, the same arguments its launcher uses) through the ghost
  desktop launcher, with its own debug switches `AC_INPUT_SCRIPT` (tap A for 4 ticks every 40 ticks from tick 340) and `AC_SHOTS`
  (a 480x270 frame every 20 presents, written at once, so each file's modified time gives that present's wall time). Window rendering
  capture is not available for its Vulkan window (PrintWindow returns white), so milestones were found by comparing shots with
  reference thumbnails of the title, map and ride HUD, then checked on contact sheets. Process start comes from the OS creation time,
  present 1 from its own log (`guest starts N ms after process start` plus `world clock starts at present 1, X s after the guest started`).
  Settings forced: windowed, 60 fps, render scale `game` (1920x1080), upscaling and anti-aliasing off, frame generation off, HD shadows
  off, texture pack off.
- Ours: `host/run_boot.ps1 -Route boottime` from a worktree build of `origin/main`; milestones from the log (`present #1`, the route
  waits `ui=title`, `ui=map_select:StageSelect`, `ride`). `SNAP_WINDOW_H=1080`, default 60 fps, no upscaler, no mods, route taps A every 40
  presents (hold 4) from the title event and every 120 presents (hold 8) once the map is open (40/4 does not answer the map's
  area panel).
- Cache states. Ours: cold = translated-shader cache present (as the builder leaves it), pipeline cache empty, the 1,172-record pipeline
  seed on; warm = the pipeline cache the cold run saved. Rival: cold = after its first run's caches were restored (`pipeline-keys.bin`,
  `pipeline-cache.bin`, `shader-answers.bin`, `shader-programs.bin`, `shader-cache`); warm = the launch right after a cold one.
  The first-ever row empties all of them (ours: empty `nvn_cache` and pipeline cache).
- Not equalized: the rival's present mode is mailbox and its audio device opens (ours null audio); its window is 3840x2130 on the hidden
  desktop (ours 3840x2160; both render 1920x1080); the OS file cache stayed warm (7.3 GB image, large RAM), so "cold" means shader and
  pipeline caches, not a cold disk; ours is timed from the exe's own log clock (about 0.1 s after process creation) and the rival
  from process creation; "title" is a log event for us and a 0.33 s-resolution shot for them; "ride started" is rail movement for us and
  the first ride HUD frame for them; input cadence differs after the map; the rival also pays its launcher (0.5-1.6 s) in normal use,
  which is not in its rows. Our launcher steps were not measured.
