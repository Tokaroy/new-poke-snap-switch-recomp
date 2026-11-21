# Developer loop (SNAP_DEV=1)

Live tuning, GPU pass timing and shader hot reload for the renderer. Everything is **off unless `SNAP_DEV=1`**; with it unset each API
call is one cached-bool branch, no thread, file or allocation exists. Files: `runtime/nvn/nvn_dev.h` / `nvn_dev.cpp` (API, control channel,
watcher), `runtime/ui/ui_tune.cpp` (F1 "Tuning" tab, overlay, shader errors), glue in `nvn_perf.cpp`, `nvn_vk.cpp`, `ui_overlay.cpp`.

## API (include `nvn_dev.h`)

| Call | Meaning |
|---|---|
| `dev_param_f(name, &x, min, max, def)`, `dev_param_i(...)`, `dev_param_b(name, &b, def)` | register a live value. `name` = the env var that sets the same value. Pointer must live for the process. A set env var seeds the value. Read the variable every frame; a change shows next frame. |
| `dev_envf(env, def, min, max)`, `dev_envi(...)` | drop-in for `EnvF("SNAP_X", def)` read per frame: same as getenv when off, a live param when on. |
| `dev_spv(name, words, sizeof words)` -> `{words, bytes}` | SPIR-V for shader `name`: `SNAP_SHADER_DIR/<name>.frag|.comp|.vert` compiled with the pinned glslangValidator, else `<name>.spv`, else the embedded copy. |
| `dev_spv_changed(name, &seen)` | true once per reload, per consumer (`uint32_t seen = 0;` next to the pipeline). Rebuild the pipelines using `name` when it returns true, then call `dev_spv` again. Old pipelines are not destroyed (command buffers may still use them). |
| `dev_pass_begin(cb, name)` / `dev_pass_end(cb, name)` | GPU timestamps in the frame's command buffer. Time up to the next mark is booked to `name`. Only the cb the renderer is replaying into counts. |

Example (lighting Fx):

```cpp
static float g_aoRadius = 1.0f;                 // existing variable
dev_param_f("SNAP_LIGHT_AO_RADIUS", &g_aoRadius, 0.1f, 4.0f, 1.0f);
...
dev_pass_begin(cx.cb, "lfx ao");  DrawTo(...);  dev_pass_end(cx.cb, "lfx ao");
...
static uint32_t aoGen = 0;
if (dev_spv_changed("lfx_ao", &aoGen)) { auto s = dev_spv("lfx_ao", kLfxAoFrag, sizeof kLfxAoFrag); g.fsAo = FxMakeModule(h, s.words, s.bytes); g.pipeAo = ...; }
```

## Keys and switches

- **F1 > Tuning**: sliders and toggles for every registered param, grouped by name, filter box, `reset` per changed row, "Copy changed as env line"
  (`SNAP_A=1 SNAP_B=2`), "Copy as PowerShell" (`$env:SNAP_A='1'; ...`), "Save env file" (`build/ctl/tuned_<pid>.env`).
- **F4**: GPU cost overlay (top-left, ms per named pass averaged over 1 s, GPU busy on top). Host overlay: never in the game's photos or album
  screenshots; `NVN_FRAME_DUMP_POST` dumps (finished swapchain image) do show it. Not toggled when F4 is bound to a game button.
- Shader compile errors: red box at the bottom-left, previous shader keeps running, until it compiles again.
- Env: `SNAP_DEV=1`, `SNAP_SHADER_DIR=<dir>` (e.g. `runtime\nvn\shaders`), `SNAP_GLSLANG=<exe>` (default: `tools\vulkan_sdk\Bin\glslangValidator.exe`
  found upward from the shader dir), `SNAP_DEV_CTL=<dir>` (default `<nearest ancestor named build of the exe>\ctl`), `SNAP_DEV_OVERLAY=1`,
  `SNAP_DEV_TIMING=1` (timestamps on, no overlay), `SNAP_DEV_RENDERER_MARKS=1` (also book the renderer's own `rp`/`cs`/`aa`/`fx` marks; many, they cost GPU time).

## Control channel (scripts)

Directory `build/ctl/` (`dev_ctl_dir()`). Files per game process id `<pid>`:

| File | Direction | Content |
|---|---|---|
| `<pid>.info` | out | written at start: pid, protocol 1, exe dir |
| `<pid>.cmd` | in | commands, one per line (`#` comments). Write `<pid>.cmd.tmp` then rename to `<pid>.cmd` so the game never reads half a file. Polled every 100 ms. The game **deletes it as soon as it has read it**. |
| `<pid>.out` | out | one answer block per processed command file (replaced each time), lines starting `ok ...` or `err ...`. Wait for `.cmd` to vanish, then read it. |
| `<pid>.passes` | out | rewritten once a second while timing is on: `gpu_busy_ms<TAB>x` then `pass name<TAB>ms` largest first |

Commands: `set NAME VALUE` (clamped; bool takes 0/1/on/off), `get NAME`, `reset NAME`, `list` (name kind value min max default), `passes`,
`overlay on|off`, `timing on|off` (timestamps without the overlay), `renderer on|off`, `reload [SHADER]`, `shaders`, `ping`.

PowerShell:

```powershell
$pid_ = 12345; $d = 'A:\AI\Recomps\new-pokemon-snap\build\ctl'
"set SNAP_LIGHT_AO_RADIUS 1.5`ntiming on" | Set-Content "$d\$pid_.cmd.tmp"; Move-Item "$d\$pid_.cmd.tmp" "$d\$pid_.cmd"
while (Test-Path "$d\$pid_.cmd") { Start-Sleep -Milliseconds 50 }; Get-Content "$d\$pid_.out"
```

## Hot reload

With `SNAP_DEV=1` and `SNAP_SHADER_DIR` set, `dev_spv` compiles `<name>.frag|.comp|.vert` at first use. The folder is polled 4 times a second: a
changed source (or a changed `.glsl` / `.h` include, which rebuilds every watched shader) must hold still for one scan, is compiled by a background
glslangValidator process (BelowNormal, hidden) and applied at the next frame boundary (`dev_frame`, present thread). The log shows
`[dev] shader <name> reloaded live in N ms (generation G)`; on an error it shows `[dev] shader <name>: COMPILE ERROR, keeping the previous shader`
and the red box. Consumers must poll `dev_spv_changed`. Dropping a `<name>.spv` in the folder also works (for a shader without a source file).

## Scripts: use a private control directory

Control files are named by game process id, and every SNAP_DEV game on the PC writes to the same `build/ctl`. A script that picks "the newest
`.info`" can talk to another session's game (this happened once: a `set` reached a stranger's run). Start your own game with
`SNAP_DEV_CTL=<private dir>` and talk to that directory only, or take the pid from your own `run_boot` / `snap_boot` process.

## Cost

SNAP_DEV unset: one cached-bool test per call, nothing else (checked on the present path: `if (dev_on()) dev_frame(...)`). Overlay or timing on:
timestamp pairs around each marked pass, one query pool read per present.

Reference build with every dev branch folded away: `host\build_snap.py --out build\w\<you>_off --cflags-extra=-DSNAP_DEV_DISABLED` (`dev_on()`
becomes `constexpr false`). Use it as the A side of an A/B to show the unset case costs nothing.

## Verified (2026-11-15)

F1 > Tuning, F4 overlay and the red error box, in a hidden Florio ride at 4K (screenshots in `analysis\devloop\`, local only). Hot reload of
`lfx_ao.frag` while the game ran: `[dev] shader lfx_ao reloaded live in 122 ms (generation 1)`, `[dev] lighting: shader lfx_ao applied (pipelines
rebuilt)`; a bad edit logged `COMPILE ERROR, keeping the previous shader` and showed the box; the fixed file applied again (generation 3).
Control channel: `ping`, `list`, `passes`, `shaders`, `set`, `get`, `reset` all answered.
