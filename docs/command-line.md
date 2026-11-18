# Command line

Every option the launcher offers for starting the game also has a flag, so another launcher, a shortcut or a script can start Snap Recomp without our window. `SnapRecomp.exe --help` prints the same list.

```
SnapRecomp.exe                      open the launcher
SnapRecomp.exe --play [options]     start the game without the launcher window
```

## Options

| Flag | What it does |
| --- | --- |
| `--play` | Skips the launcher window. The game-folder check still runs first. The process ends when the game closes, so the caller can wait on it. |
| `--safe-mode` | One start with mods, HD packs, enhanced effects and the shader cache off ([safe-mode.md](safe-mode.md)). |
| `--fullscreen`, `--borderless` | Borderless full-screen window. Both names do the same thing. |
| `--windowed` | Normal window. |
| `--monitor <n>` | Monitor number. 0 is the first. |
| `--width <px>`, `--height <px>` | Window size. |
| `--profile <name>` | The profile to play, or the one `--import-save` and `--export-save` use. |
| `--set <key>=<value>` | One setting for this start only. Repeat it for more settings. The settings file is not changed. |
| `--data-dir <folder>` | Keeps saves, settings, mods, caches and logs in this folder. The folder is created when it is missing. |
| `--help`, `-h`, `/?` | Prints the flag list and exits with code 0. |
| `--version` | Prints the version and exits with code 0. |

Without `--play` the launcher window opens with the flags already applied, and they hold for the game it starts.

When two window flags disagree, the last one on the line wins. A flag wins over an environment variable, and an environment variable wins over the settings file.

A mistyped flag stops the start: the exe prints `SnapRecomp: unknown option --name (see --help)` and exits with code 2. A flag with a missing or bad value does the same.

## Examples

```
SnapRecomp.exe --play --fullscreen --monitor 1
SnapRecomp.exe --play --windowed --width 1280 --height 720 --profile Ash
SnapRecomp.exe --play --set fps_target=120 --set fps_overlay=fps
SnapRecomp.exe --play --data-dir D:\SnapData
```

## Keys for `--set`

`--set` takes the same keys the settings file (`settings.cfg`) uses, with the same values. A name that starts with `SNAP_` sets that environment variable for the start instead, for the options that exist only as a variable.

| Group | Keys |
| --- | --- |
| Frame rate | `fps_target`, `vsync`, `frame_generation`, `gfx_full_rate_anim`, `fps_overlay`, `fps_overlay_corner` |
| Resolution and upscaling | `gfx_render_scale`, `gfx_upscaler`, `gfx_upscale_quality`, `gfx_aa`, `gfx_smaa_quality`, `gfx_sharpen`, `gfx_ultrawide` |
| Detail | `gfx_aniso`, `gfx_lod_bias`, `gfx_draw_dist`, `gfx_lod_dist`, `gfx_lod_force0`, `gfx_pom`, `foliage_aa`, `leaf_translucency`, `soft_particles` |
| Lighting and shadows | `gfx_lighting`, `gfx_bounce`, `gfx_shadow_res`, `gfx_shadow_dist`, `contact_shadows`, `ssao_quality`, `gfx_ssao_samples`, `gfx_ssao_hbao` |
| Ray tracing | `rt_reflect`, `rt_gi`, `rt_gi_rays` |
| HDR | `gfx_hdr`, `gfx_hdr_peak`, `gfx_hdr_paper`, `gfx_hdr_expand` |
| Water | `water_ssr`, `water_caustics`, `water_foam`, `water_ocean` |
| Atmosphere and lens | `atmos_shafts`, `atmos_fog`, `atmos_exposure`, `atmos_bloom`, `atmos_clouds`, `atmos_sky`, `gfx_bokeh_dof`, `gfx_motion_blur`, `film` |
| Interface | `hud_scale`, `gfx_ui_font`, `ui_smooth`, `game_language` |
| Photos | `photo_export`, `photo_dir`, `hd_photos` |
| Play | `fast_boot`, `skip_cutscenes`, `skip_course_intro`, `neo_speed`, `freecam`, `freecam_freeze`, `spawn_rand`, `spawn_seed`, `return_to_launcher` |

Other keys from `settings.cfg` work too. Safe mode still ignores the settings it always ignores, even when `--set` names one.

## Data folder

`--data-dir <folder>` puts everything the game writes under that folder: `Roaming\SnapRecomp` holds saves, settings, profiles and mods, and `Local\SnapRecomp` holds caches and logs. Nothing is copied from the normal data folder, so a new folder starts with a fresh save. Import one with `--import-save` and the same `--data-dir`.

A `portable.txt` file next to the exe does the same with a `portable_data` folder beside the exe. `--data-dir` wins when both are present.

## Other commands

These do one job and exit. None opens the launcher.

| Command | What it does |
| --- | --- |
| `--import-save <folder or zip>` | Copies a save into the profile. Exit code 0 for success, 1 for a failure, 2 when the game is running. |
| `--export-save <folder or zip>` | Writes the profile's save out, with the same exit codes. |
| `--benchmark` | Plays the benchmark ride and writes a report. `--benchmark-capped` and `--benchmark-uncapped` pick the frame-rate mode, and `--benchmark-view <file>` shows an earlier report. |
| `--prepare-shaders` | Builds the shader cache ahead of play. Takes `--force`, `--visible`, `--list`, `--stop` and `--courses <list>`. |

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | The game or the command ended normally |
| 1 | A command failed |
| 2 | A bad command line, a data folder that could not be created, or converted game files that are missing or damaged |
