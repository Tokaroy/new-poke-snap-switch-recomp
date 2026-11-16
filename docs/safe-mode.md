# Safe mode

One start with everything optional switched off, for a game that will not get to the title screen. Your settings, mods and save are not changed.

## What it turns off

| Part | In safe mode |
| --- | --- |
| Mods and HD packs | no pack is loaded, so no file is replaced (`[mods] safe mode: no packs loaded`) |
| Enhanced effects | every saved graphics, effect and camera setting is ignored; the run uses the original look (`SNAP_VISUALS=original`) |
| Frame generation, upscaler, ray tracing | off, even when set by hand in the environment |
| Shader cache | a separate empty folder (`shadercache_safe`) is used, so your normal cache is never read or changed |

Only settings that do not change what is drawn or loaded are kept: game language, accessibility, button icons and swap, gyro and rumble, HUD size, window size. The settings file on disk is not rewritten during the run, so changes made in the F1 menu are lost at exit. Frame rate stays at the game's own 60.

A one-line banner at the top of the window says the run is in safe mode. It is drawn by the host overlay and never reaches a photo. The log has `[safe-mode] ON for this run: ...` at the start.

## How to start it

- Launcher: the **Safe mode (X)** button on the Play card, or X while Play is selected.
- Command line: `SnapRecomp.exe --safe-mode` (also opens the launcher first), or `snap_boot.exe run --safe-mode` for a development run.
- Offered by itself: when the last start died before the title screen, the launcher asks "Start in safe mode?" before it opens.

## How the offer works (launch marker)

Each start writes `launch.marker` next to `_crash.txt` (the logs folder, or `SNAP_CRASH_DIR`) with its process id, start time and whether it was a safe-mode start. The marker is removed when:

- the title screen opens (the `title` scene, or `main_menu` for a start that skips it), or
- the run ends cleanly (window close, or the `--timeout` exit).

A marker left behind whose process is gone means the last start never got to the title screen. The launcher then says whether it crashed (`_crash.txt` is newer than the marker) or just stopped, for example when it was killed while hanging. A marker whose process is still running (a second copy) is ignored. If the last start was already a safe-mode one, the question says so.

Answering No starts the normal launcher. The marker stays until the next start writes a new one. Test runs keep their marker in the run folder, so they never reach the launcher's offer.

Test switches: `SNAP_LAUNCHER_AUTOPLAY=1` skips the question and logs `launcher: safe mode offered: ...`; with `SNAP_LAUNCHER_SAFE_ANSWER=yes` it also starts in safe mode. `SNAP_CRASH_TEST=av|abort|gpu|terminate` (see `docs/crash-logs.md`) makes a start that dies at present 120, before the title screen.

Code: `host/mods/crashlog.cpp` (`snap_launchmark_*`), `host/mods/routes.cpp` (title scene), `host/launcher.cpp`, `runtime/hle/settings.cpp` (`safe_mode`), `runtime/mods/mods.cpp`, `runtime/nvn/nvn_pipeline.cpp`, `runtime/ui/ui_fps.cpp` (banner).
