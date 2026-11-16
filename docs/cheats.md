# Cheats

Added 2026-11-15. A Cheats tab in the F1 menu lists the cheats from a cheat file you put next to the game, with one switch per cheat. It is off by default: until you turn the master switch on, the cheat file is not even read and the game runs exactly as shipped, photo scores included.

The project ships no cheats and no cheat file. You write your own, or use files you trust. The format is the Atmosphere cheat text format (sections `[Name]` and `{Master}`, lines of eight-digit hex words).

## Turning it on

| What | Env var | Settings key | Values |
| --- | --- | --- | --- |
| Master switch ("Allow cheats" in F1 > Cheats) | `SNAP_CHEATS` | `cheats_enabled` | `0` / `1`; off by default |
| Enabled cheats | `SNAP_CHEATS_ON` (test runs) | `cheats_on` | the enabled names joined with `\|` (the menu writes it) |
| Folder with the cheat files | `SNAP_CHEATS_DIR` (test runs) | none | default: `cheats\` next to the exe |

The file for this game build is `cheats\<build id>.txt` next to `snap_boot.exe`. The build id of the 2.0.1 main module is `DDB900FF04A912ED42430508389853CFAF1ACCCE`; the file name can be the whole id or its first 16 characters (`DDB900FF04A912ED.txt`, the usual name). The Cheats tab shows the exact path it looked at, and "Read the file again" re-reads it after you edit.

Sections in `[brackets]` are ordinary cheats with their own checkbox, off until you tick them. Sections in `{braces}` are always-on cheats: they run whenever the master switch is on and cannot be unticked (a menu line says so).

## What runs, and when

The VM (`runtime/hle/cheat/`, a standalone Atmosphere-format interpreter with its own tests) runs once per presented frame, on the game's own thread, from the Npad state read in `runtime/hle/hid.cpp`. The glue is `runtime/hle/cheats.{h,cpp}`:

- Memory: the VM only calls back into an adapter. Guest addresses are host addresses in this build, so the adapter checks every access with `VirtualQuery` first (committed, readable, and writable for stores). A cheat that aims outside mapped memory fails its run with the line number in the log and the Cheats tab; it cannot crash the game. Regions: main module and ASLR base `0x7100000000`, heap from the game's own heap bookkeeping (`runtime/os/cheat_heap.cpp`). This game never sets a memory heap and maps no alias region (log line `cheats: memory heap base 0`), so a cheat addressed from the heap or the alias region fails and is switched off; cheats addressed from the main module (the usual pointer chains) work.
- Keys: `keys_held` is the same pad state the game reads (after your bindings and the accessibility toggles), in the Switch key layout the cheat format uses (A = bit 0, B = 1, X = 2, Y = 3, L3 = 4, R3 = 5, L = 6, R = 7, ZL = 8, ZR = 9, Plus = 10, Minus = 11, Left = 12, Up = 13, Right = 14, Down = 15).
- When a cheat fails (a bad address, a bad opcode) the run for that frame stops at the line, the log names the cheat and the line once, and the cheat is switched off for the rest of the session (its checkbox clears; the saved list is unchanged, so a reload tries it again). The cheats after it keep running. An `{always-on}` section that fails cannot be switched off and stops the cheats after it every frame, so fix the file.
- Pause and resume callbacks do nothing: the VM already runs at a safe point on the game thread.
- Debug-log opcodes write `cheat: debug log <id> = <value>` to the log.
- One run is limited to 100000 instruction steps.

## Photo scoring and the other rules

With the master switch off, nothing in the cheat code runs. With it on, only the cheats you enabled (and any `{always-on}` sections) touch memory, and they touch only what they say. A cheat that changes a photo, a Pokémon or the research level changes the score; that is the point of such a cheat. The F1 tab says so, and the problem report records whether cheats were on (below).

## Problem report

The problem report's `system.txt` has a `Cheats active: yes/no` line. `yes` means the master switch was on and the file has an always-on section or an enabled cheat.

## Checks

- HLE test `cheats.wiring` (`tests/hle/cheats_tests.h`): hand-written text, a 64 KiB buffer standing in for the heap. Covers the master section, an off cheat doing nothing, the saved enabled-names list, a key-conditional store with the key up and down, a store past the mapped range failing the run without a crash, a parse error naming its line, and the master switch and `Cheats active` following `SNAP_CHEATS`.
- VM suite: `runtime\hle\cheat\build.bat` (synthetic words only).
- Tab: `tests/hle/run_cheats_ui.ps1` (F1 > Cheats on screen, two `expect` checks).
- In game: `host/routes/cheat_probe.txt` with the hand-written file `tests/cheats/DDB900FF04A912ED.txt` (a store that writes back the value it just read, and a ZL-conditional debug log). Results are under "Verification results".

## Verification results

Run 2026-11-16 on a build with the cheats wiring, route `cheat_probe`, `SNAP_CHEATS=1`, the hand-written file `tests/cheats/DDB900FF04A912ED.txt` (both cheats enabled by `SNAP_CHEATS_ON`). Log of the run: `analysis/boot/runs/2-20261116_010419/boot_stderr.log` (local).

- `cheats: Loaded 2 cheats from cheats\DDB900FF04A912ED.txt.` once, at the first frame.
- The ZL-conditional cheat wrote `cheat: debug log 7 = 0xf81d0ff5 (4 bytes)` 593 times, only inside the ZL bursts of the route (the key bits come from the Npad state), and the value is the real first word of the function at main + 0x68270.
- The heap store cheat failed at its first run (`"No-op store in the heap" failed at line 11: memory register load failed and was switched off`) because this game never sets a memory heap (`cheats: memory heap base 0`). It was switched off, logged once, and the ZL cheat in the same file kept running. No crash.
- With `SNAP_CHEATS` unset (the two evaluation runs of `docs/accessibility.md`), the log has no `cheats:` line at all.
- HLE tests: 2074 checks, 0 failed (`cheats.wiring` included); VM suite: 269 checks, no warnings at /W4.

- The F1 Cheats tab (`tests/hle/run_cheats_ui.ps1`, 2 checks pass, frame `analysis/ui/cheats_tab.png`, local): the "Allow cheats" switch is on, the status line and file path show, "Read the file again" is there, the ZL cheat is ticked, and the heap cheat is unticked because it had already been switched off after its failure.

Not covered in the game: a cheat that writes to main-module data. The probe file has none, so nothing in it changes the game's own state.

## Not done

- A row in the game's own Settings screen. That screen is data driven with fixed rows; a list that comes from a file at run time does not fit it. The cheats list lives in the F1 menu, like the other tabs that are lists (Saves, Mods).
- No cheat file ships, and no downloader. Atmosphere's cheat database was not used.
