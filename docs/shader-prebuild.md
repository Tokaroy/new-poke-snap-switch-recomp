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
