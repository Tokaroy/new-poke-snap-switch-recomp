# Mod hooks

A mod hook runs your host code before or after a named game function. Hooks need no regeneration of `main`: the loader swaps the function's entry in the dispatch table for a trampoline. Source: `runtime/hle/modhooks.{h,cpp}` (core), `host/mods/modhooks.cpp` (point list, samples, install). Test: `modhooks` in `tests/hle/hle_tests.cpp`.

## Turning a hook on

Hooks are off until you turn them on. Nothing is installed for a point nobody enabled, so with no hook on the game runs the code it always ran: no check, no call, no cost per game function call. The only startup cost is one file open and one `getenv`.

`<mods root>\hooks.json` (the mods root is `SNAP_MODS_DIR` or `%APPDATA%\SnapRecomp\mods`, the same folder as `mods.json`):

```json
{ "hooks": {
    "sample-ui-log/Ui.SetState": true,
    "sample-ride-camera/Camera.RideBuild": false
} }
```

- A key is `<mod id>/<point name>`. `//` comments are allowed.
- A hook that is not listed is off, unless its mod registered it as default on. The manifest can still turn it off.
- A manifest that is not valid JSON, or has a value that is not `true` or `false`, is refused: no hook is turned on for that start, and the log says why.
- `SNAP_MOD_HOOKS=<list>` applies after the manifest: comma separated `<mod>/<point>` entries, `-` in front turns one off, `*` turns on every registered hook. It is for tests.
- Changes apply at the next start.

At start the log says what was installed:

```
[modhooks] Ui.SetState (main+0x79a6d0): 1 hook(s) on; no direct callers, every call is seen
```

## Point names

Names are case sensitive and never change or move once listed here. New points are appended. Offsets are `main` module offsets. "Before" hooks see the arguments in `c->x[0..7]` and may change them; "After" hooks see the result in `x0`. Registers other than x0 to x7 are not reliable in an After hook.

| Point | main offset | At the call |
| --- | --- | --- |
| `Camera.RideBuild` | `0x295170` | x0 = camera object. The ride camera builder, once per frame of a ride. After it ran, eye / target / up are at x0+0xd0 / +0xe0 / +0xf0 (floats). |
| `Ui.Event` | `0x6c1d90` | x1 = `int*` event id, x2 = `Player**`, x3 = `std::string*` state. The UI scene base's `std::function` call operator; events 18 and 10 are a decide press, 22 is the per-frame event. |
| `Ui.OptionEvent` | `0x728e10` | x3 = `std::string*` tab or element name. The option screen's event handler. |
| `Ui.SetState` | `0x79a6d0` | x0 = player, x1 = page descriptor; the state name is a libc++ `std::string` at x1+0x18. `Player::SetState`. |
| `Font.AcquireGlyphmap` | sdk `0x160a80` | w1 = the Unicode codepoint (x0 = the font engine, w2 = a size). `nn::fontll::ScalableFontEngine::AcquireGlyphmap`; an sdk offset, not a main one. The bundled `button-icons` hook (on by default) changes the Switch button letters U+E0E0 to U+E0E3 in message text to the letters of the pad buttons that do the action (Xbox style: the game's "X" reads Y). `SNAP_BTN_TEXT=0` turns it off. |
| `Font.CheckGlyphExist` | sdk `0x160a90` | w1 = the codepoint. `ScalableFontEngine::CheckGlyphExist`; the same `button-icons` hook, so the swapped letter is looked up the same way. |

A libc++ `std::string` is short when byte 0 is even (size = byte 0 / 2, text at +1) and long when bit 0 of byte 0 is set (size at +8, data pointer at +0x10). `read_str` in `host/mods/modhooks.cpp` shows it.

### What a hook sees: direct calls are not hooked

The dispatch table is read by indirect calls: virtual calls, function pointers, `std::function`, and calls from `main` into `sdk` through the PLT. The lifted code calls a function in the same module with a plain C call (`f_<addr>(c)`), which does not read the table, so a hook there would miss those calls. Every point in the list has no direct caller in `main` (checked by counting `bl` / `b` to the function in the generated code and, at start, in the mapped text). If you add a point that has direct callers, the start-up log says how many it will miss:

```
[modhooks] Rail.Update: 2 direct caller(s) in main skip the dispatch table and are not hooked
```

To also catch direct calls a point needs an inline hook site in `recomp/snap_gen.py` (`MAIN_INLINE_HOOKS`) and a regeneration of `main`; that costs a call per function call for everyone, which is why the table approach is the default.

## Writing a hook

A hook is a C function `void fn(Cpu* c, void* user)` registered with `hle::modhooks::add` before the point is installed (in `snap_modhooks_install`, `host/mods/modhooks.cpp`):

```cpp
mh::Hook h;
h.mod = "my-mod";            // letters, digits, - _ .  (up to 48)
h.point = "Ui.SetState";     // a name from the table above
h.when = mh::When::Before;   // or After
h.fn = my_fn;
h.default_on = false;        // true: on without a manifest entry
mh::add(h);
```

`add` returns false for an unknown point, a bad mod id, a null function, a second hook with the same mod and point, or a point that is already installed.

Rules for the function:

- It runs on the game thread that made the call, so it may run on several threads at once. Keep state in atomics or behind a lock.
- It must be short. The game waits for it.
- Reading guest memory: guest pointers are host pointers. Check them (`IsBadReadPtr`) before reading anything the hook did not just get from the game.
- Calling guest functions: use `loader::call_guest_nested`, not `call_guest`.
- Before hooks may change `c->x[0..7]`; the original function sees the change. After hooks may change `x0`; the caller sees it.
- If an exception or `longjmp` passes through the original function (`c->resuming` set), After hooks do not run.

## Crash guard

Every call of a hook runs under a guard. A hook that faults (access violation, divide by zero, a thrown C++ exception) is switched off for the rest of the run. Before the call the guard saves x0..x30, sp, pc and the flags; after a fault it puts them back, so the original function sees the registers it would have seen. The log names the hook and the fault once:

```
[modhooks] sample-fault/Ui.Event faulted (access violation writing 0x0 at 0x7ff6...): hook disabled for this run
```

The guard cannot undo what the hook wrote to guest memory before it faulted, and it does not save the vector registers (v0 to v31). A hook that loops forever still hangs the game.

## Sample mods

Bundled in `host/mods/modhooks.cpp`, all off by default.

| Mod / point | What it does |
| --- | --- |
| `sample-ui-log/Ui.SetState` (Before) | Logs the first 300 UI page changes: `[sample-ui-log] #3 Ui.SetState player=0x... state=...`. |
| `sample-ride-camera/Camera.RideBuild` (After) | Logs the ride camera eye once per 60 builds. |
| `sample-fault/Ui.Event` (Before) | Reads a null pointer on its first call, to show the crash guard. |

Try one: `SNAP_MOD_HOOKS=sample-ui-log/Ui.SetState host\run_boot.ps1 -Route <route>`, then search the log for `[sample-ui-log]`.

## Tests

`tests\hle\run_tests.ps1` runs `modhooks`: nothing enabled leaves the cell untouched; manifest and environment switches; registration rules; before, original, after order and register changes; a hook that faults or throws is disabled with its register writes undone while the original and the next hook still run; After hooks skipped while `c->resuming` is set. The game-side install is checked by a route run with a sample hook on (see `host/README.md`).

## Not done

- Native plugin DLLs from a pack folder. Packs hold data only today; loading code from a downloaded pack is the owner's call.
- Hooks on `sdk` functions. The dispatch-table swap and the import cache refresh work for them (`loader::dtab_cell`, `loader::refresh_import_fns`), but a function the host already overrides by name (the photo export, for one) is bound to the host thunk instead of the lifted function and cannot be hooked this way.
- Replacing a function's result without running it (a "Replace" mode). Before and After cover the common cases; a replace hook can be built on a Before hook that sets the registers and a flag the After hook reads.
