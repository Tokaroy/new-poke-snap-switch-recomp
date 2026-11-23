# Shader prebuild (prepare shaders)

The first visit to a course used to stutter while the pipeline service compiled each new graphics pipeline. Prepare shaders removes that: it plays one scripted ride per course in a hidden copy of the game, so the pipeline service records and compiles every pipeline into this PC's shader cache before you play.

Nothing game-derived is shipped. The key list, the driver cache and the progress files are made on the player's own PC, in `<data dir>\shadercache` (or `SNAP_SHADERCACHE_DIR`).

## How it works

1. `SnapRecomp.exe --prepare-shaders` (`host/shaderprep.cpp`) starts one second copy of the game per course, one after another.
2. Each copy runs on the benchmark's scratch save profile (the real saves are never touched), on a desktop that is never shown, at below-normal priority, muted, with the ride sped up (`SNAP_PREP_SPEED`, default 3).
3. The route is the HD-texture capture route for that course (`builder/hdtex_routes/qa_<course>_e0.txt`), packed into `host/prep_routes_data.h` by `scripts/prep_pack.py`. It goes from the title screen through the lab and the course map, rides the course while tapping the shutter, and stops at the photo-submit screen. Florio keeps its photo-evaluation tail, because the evaluation passes are the same for every course.
4. The pipeline service (`runtime/nvn/nvn_pipeline.cpp`) appends each new pipeline to `pipeline_keys.bin` and compiles it, as it does in normal play. Next launch, `SNAP_SHADER_PRECOMPILE` replays those keys.
5. After every course the runner writes `prepare.json` (state, course index, pipelines known). The launcher's shader chip reads it: "Preparing shaders 3 of 14", "Shaders prepared (N pipelines)".

## Launcher

The shader chip on the home screen is a button. Press it to start the sweep, press it again to stop after the current course. The sweep runs in its own process, so the launcher can be closed. A full sweep took 41 minutes on the development PC while three other test runs shared it; expect 30 to 40 minutes.

## Command line

```
SnapRecomp.exe --prepare-shaders                  all 14 courses
SnapRecomp.exe --prepare-shaders --courses a,b    only these keys
SnapRecomp.exe --prepare-shaders --list           print the course keys
SnapRecomp.exe --prepare-shaders --visible        show each ride (debugging)
SnapRecomp.exe --prepare-shaders --stop           stop after the current course
SnapRecomp.exe --prepare-shaders --force          run again when already prepared
```

A development build without `config.json` passes extra arguments on to the game (`--main ... run`). `SNAP_PREP_TIMEOUT=<s>` limits one course (default 900). `SNAP_PREP_KEEP=1` keeps the scratch folder and the per-course logs.

## Builder

`snap_builder.py build --prepare-shaders` (off by default) starts the sweep in the background right after the new build is promoted, so a fresh install has its cache after about half an hour. The builder does not wait for it; the launcher chip shows progress. The setup wizard does not turn it on: the chip on the launcher home screen is the normal way.

## Cache identity and wipe

`cache_stamp.txt` in the cache folder holds two lines.

| Line | Contents | Changes when | Wiped |
| --- | --- | --- | --- |
| build | translator hash, game build hash, `PipeDesc` size, file format | the recompiled game, the translator or the key layout changes | `pipeline_keys.bin`, `pipeline_cache.bin`, `prepare.json`, `status.json` |
| GPU | vendor, device, driver version, pipeline-cache UUID | a new GPU or driver | `pipeline_cache.bin` only (the keys stay valid, the driver blob does not) |

A missing stamp is not a wipe: an existing cache from an older build is adopted and stamped. After a wipe the chip goes back to "press to prepare".

### At start

- Nothing changed (same GPU, driver and build, driver cache loaded): the game starts at once. The key list is replayed in the background as cache hits (about 1,000 pipelines in under a second on the development PC); nothing compiles and no progress screen appears. The ride sweep never runs by itself.
- GPU or driver updated: the driver cache is wiped, so the first start after the update rebuilds every known pipeline. In "compile at startup" mode the game waits on the progress screen while it does (about 3 seconds for 1,000 pipelines on the development PC, longer on a driver with no disk cache of its own). The launcher chip also reads "Graphics driver changed - press to prepare shaders again", and `--prepare-shaders` without `--force` runs the sweep again.
- Game build or translator changed: the key list is gone as well, so the chip goes back to "press to prepare" and the sweep has to be run again.

`NVN_LOG=1` prints the decision ("pipeline run: N known, N to build, mode, warm start or cold start" and "N pipelines built in X ms").

`status.json` (rewritten with every cache save) lists pipelines known and ready, blob size and the two hashes.

When a full sweep ends and the player has not chosen a precompile mode yet, it writes `mode.txt` as "now" (compile at startup). Everything is already in the driver cache then, so the startup pass takes seconds and the game starts with every known pipeline built.

## Settings and the cache

The sweep plays on a scratch profile that copies the player's own graphics settings (the same rule as the benchmark). A pipeline key includes shader-affecting settings: the soft-shadow taps, the shadow-distance factor of the Enhanced look, ray-traced shadows and parallax each add bits to the key. A cache prepared with one set of settings therefore does not cover another. After changing those settings, run prepare again (the old keys stay, the new ones are added).

`prepare.json` keeps a short hash of the rows that matter (`settings`: the graphics, ray-tracing, atmosphere, water, foliage, leaf and effect rows of the player's `settings.cfg`, not sound, window or control rows). The launcher compares it with the current settings at every refresh. When they differ the chip reads "Settings changed - press to prepare shaders again" and the next press runs a new sweep. `--prepare-shaders` without `--force` also runs again when the hash differs.

## Limits

- A pipeline depends on state that is only known at draw time, so it cannot be listed ahead of time. The routes cover what a normal ride draws. A rare path (an unusual photo subject, a mod that adds materials) can still compile once on first use; the pipeline service handles that the way it always has.
- The sweep needs the game files and a working GPU driver. It stops with `failed` in `prepare.json` if a course exits with an error or runs past the time limit.

## First launch: shaders translated in the background

The game creates all of its shader programs while it boots (about 6,200 programs from about 1,650 distinct shaders). A program whose shader
is not in the translated-shader cache (`nvn_cache`, in `portable_data\Local\SnapRecomp\` beside a portable exe) needs the translator
(`shader_harness`). That used to happen inline, one shader after another, on the thread that loads the game, so the first launch waited for
all of them before the title screen.

Now (`runtime/nvn/nvn_shader.cpp`, `ProgramJob`):

1. A program whose shaders are all in the cache is built at once, as before.
2. Any other program is queued and the game carries on. Worker threads, each with its own translator process (`xlate::Pool` in
   `runtime/nvn/nvn_xlate_server.h`), build the queue in order.
3. When the game records a bind of a queued program, that program moves to the front of the queue.
4. Nothing is drawn without its shader. Every use of a program (the draw replay, the persistent draw state, destroying or re-creating it)
   first waits for it, and a program still queued at that point is built on the waiting thread at once. Draws are never skipped for a
   shader, so a photo cannot catch a missing one.
5. The translation and the files it writes are exactly those of the inline path: same translator, same request, same key, same publish
   by rename. Only the order and the number of processes change.

`--shader-workers <n|auto>` (variable `SNAP_XLATE_WORKERS`) sets the number of workers. `auto` (default) is half the logical processors,
at most 8. `1` is the old behavior: one process and inline translation. The log prints `background shader build: done, N programs built
by W workers in S s` each time the queue empties.

Measured on the development PC (32 logical processors, other test runs sharing it), route `boottime_first`, empty translated-shader and
pipeline caches, seconds from start to the title (median of 3, interleaved): one translator inline 28.8 s, 8 background workers 14.3 s,
a warm translated-shader cache 15.8 s (details in `docs/boot-time-vs-rival.md` "First-ever launch after the change"). The pipeline cache is
not a factor before the title: 37 pipelines, about 0.1 s of compile in total.

Why not translate during the build instead: the release builder has no Oodle decoder, so it cannot read the shaders out of the game's
archives without starting the game, and the game boot now does the whole set in a few seconds anyway. The builder's smoke run (the
first boot, to the title) gets faster by the same amount.

## Pipeline libraries (`gfx_gpl`, experimental, off)

Setting `gfx_gpl` = `auto` | `on` | `off` (env `SNAP_GPL`, flag `--gpl`, F1 Graphics "Pipeline libraries"; restart). On, the device enables `VK_EXT_graphics_pipeline_library` and `pipelineCreationCacheControl`. A pipeline the game waits for (a hot job, or a must-have draw on the replay thread) is first asked from the driver with `FAIL_ON_PIPELINE_COMPILE_REQUIRED`; on a miss it is fast-linked from four libraries (vertex input, pre-raster, fragment shader, fragment output; each built once per distinct input and shared) and the whole pipeline is compiled on an idle worker and swapped in (the fast-linked one is destroyed 10 s later). Any failure builds the whole pipeline as before. The `VkPipelineCache` blob and `pipeline_keys.bin` are unchanged (libraries are created without the cache, so they never reach the blob); `kFormatVersion` stays 1. Code: `runtime/nvn/nvn_vk.cpp` (device setup, `[gpl]` log line), `nvn_draw.cpp` (`CreateGraphicsPipelineLatency`, library cache), `nvn_pipeline.cpp` (`QueueOptimize`, `RunOpt`).

**Automatic is off on every GPU** (`runtime/nvn/nvn_quirks.cpp`, feature `gpl`): `nvidia_gpl` (measured, below), `amd_rdna12_gpl` (another project's finding for RDNA 1 and 2, not reproduced here), and `gpl_unmeasured` for the rest. `on` overrides the list; a driver without the extension or without fast linking falls back to whole pipelines.

Measured on the development PC (RTX 4090, driver 621.24), Florio `florio_eval`, exclusive runs, ABBA order, empty shader cache dir, no pipeline seed (`SNAP_PIPELINE_SEED=0`). The driver keeps its own on-disk shader cache that this game cannot clear (`__GL_SHADER_DISK_CACHE=0` had no effect), so a plain cold run only measured cheap driver-cache hits (89 compiles in the course-start window at 2.6 ms on average). `SNAP_SPV_SALT=<text>` (test knob, `nvn_alpha.cpp`) adds an `OpSourceExtension` to every translated module, which makes every shader new to the driver; the salted runs below compile at 31 ms on average (172 ms at most).

| Window | | off 1 | on 1 | on 2 | off 2 |
| --- | --- | --- | --- | --- | --- |
| Course start to ride + 30 s | frames over 50 ms | 18 | 19 | 21 | 28 |
| | frames over 100 ms | 9 | 10 | 12 | 13 |
| | replay-thread pipeline wait (`getpipe_ms`) | 1,137 ms | 1,401 ms | 1,637 ms | 1,083 ms |
| Ride + 30 s | frames over 25 ms | 30 | 20 | 25 | 50 |
| | frames over 50 ms | 3 | 5 | 5 | 11 |
| Whole run | pipelines compiled or linked | 254 | 208 links + 206 whole | 203 links + 201 whole | 257 |
| | compile CPU | 7.8 s | 17.2 s | 19.3 s | 9.2 s |

Runs `0-20261123_021401` (off 1), `_022115` (on 1), `_022936` (on 2), `_023528` (off 2); all reached `ui=evaluation:End`. The game paces presents at 60 Hz with 30 Hz game frames, so "over 25 ms" counts every ordinary frame and the 50 and 100 ms counts are the hitches; they differ between the two off runs as much as between off and on (other processes on the PC, texture loads: the largest frame in the ride window, 918 to 951 ms, is not a pipeline compile). On the on runs the fast link itself took 0.06 ms; the 397 library builds took 7.4 s, the same as the whole-pipeline compiles they replace, so time to ready did not improve (the replay thread waited 37% longer on average) and the background whole-pipeline pass doubled the compile work. 435 library lookups were reused, but the libraries are built when the draw first asks for them, not before.

Warm cache (the pipeline cache and key list of an off run, mode `now`, driver cache warm): 258 pipelines rebuilt in 248 ms at start, 1 pipeline compiled in the whole ride, no fast link, no change in frame counts beyond the run-to-run spread (ride + 30 s over 50 ms: off 13 and 7, on 1 and 8; runs `0-20261123_025927`, `_031633`, `_033103`, `_034626`).

What would make it pay: build the pre-raster and fragment libraries before the draw (at program creation, with the state the key list already knows), so the draw only links. Not done.
