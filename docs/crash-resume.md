# Crash resume

Date: 2026-11-23. If the game closes without closing properly (a crash, a task kill, a power cut), the next launcher start says so and offers a way back. Nothing here changes how the game plays, the game's own save format, or photo scoring.

## What already happened after a crash (audit, before this change)

| Piece | Behavior | Where |
| --- | --- | --- |
| Save commit | A save is a numbered generation folder plus a `CURRENT` pointer. A commit writes the new generation, marks it complete with a `.ok` file, then replaces `CURRENT` through `CURRENT.tmp` after flushing it. A kill at any point leaves the previous generation or the new one, never half of each. The next mount takes `CURRENT`, else a valid `CURRENT.tmp`, else the newest complete generation, and discards unpublished ones. | `runtime/hle/fs_core.cpp`, `runtime/hle/save_gen.h` |
| Clean exit | `snap_fs_drain_for_exit` waits (up to 5 s) for a commit, screenshot or photo export in flight, so the newest photo and progress are kept. A crash skips it, which loses at most the commit in flight. | `runtime/hle/fs.cpp`, `host/boot.cpp` |
| Backups | A copy of the save is made at every start (newest 10 kept) and before every import or restore. Launcher: Restore a backup. | `docs/save-backups.md` |
| Crash report | `_crash.txt` plus `stderr.log`, `trace.log` and the `.prev` copies; the Problem report zip carries them. No minidump is written. | `docs/crash-logs.md` |
| Launch marker | `launch.marker` is written at start and removed when the title screen opens, so a start that dies before the title gets the safe-mode offer. A crash after the title was not noticed at all. | `docs/safe-mode.md` |
| Save states | Not for players. `docs/save-states.md` is a developer aid (open handles and guest state for tests), not a way to resume a ride. | `runtime/nvn/nvn_state.cpp` |

So the save itself was already crash-safe; what was missing was telling the player, and a quick way back to the menu they were in.

## What it does now

While the game runs, `session.marker` sits next to `launch.marker` (the logs folder, or `SNAP_CRASH_DIR`). It is written when the title screen opens, rewritten (through a temp file and a replace) each time a UI scene opens, and removed on every clean exit (window close, the game's own quit). It holds the process id and start time, so a second running copy is never mistaken for a crash, plus the last scene name and the time.

A marker whose owner is gone means the last session did not close. The launcher then asks "The game closed unexpectedly" with:

- **Start the game again** - the normal start.
- **Resume where you were** - skips the title screen, and for the course map also the lab menu. The launcher writes `resume_script.txt` in the logs folder and sets `SNAP_INPUT_SCRIPT` to it, so the game runs the same slow A presses a player would, waiting for the `main_menu` and `map_select` scenes (`host/README.md` "Waiting on game state"). Real input stays active. If a wait runs out the game keeps running (`SNAP_ROUTE_TIMEOUT=continue`). The target is the course map when the last scene was the map, photo select, evaluation, rewards or the album; the lab menu for any other menu; nothing when the game never got past the title. The game's own save decides which courses are open, so skipping menus never skips progress. Resume does not go back into a course: a ride has no save of its own to return to, and inventing one would mean saving guest memory (out of scope).
- **Open the problem report** - shown when `_crash.txt` is newer than the marker.
- **Restore a save backup** - shown only when the save looks damaged: a user folder with save files but no generation a mount could load, or `CURRENT` naming an incomplete generation. A kill mid-commit is not damage. It opens the existing backup list, which asks before it changes anything.

Closing the question window leaves the marker, so the question comes back next time. Any answer removes it. A safe-mode question for a start that never reached the title comes first and replaces this one.

## Settings

| Setting | Effect |
| --- | --- |
| `crash_prompt` in `config.json` (default on) | `false` turns the question and the marker off. |
| `SNAP_CRASH_PROMPT=0\|1` | The same from the environment; wins over config.json. |
| `--crash-prompt on\|off` | The same as a flag, for this start. |
| `--resume` (`SNAP_CRASH_RESUME=resume`) | After an unclean exit, resume without asking. With `--play` the question is never asked: without `--resume` that is a normal start and a log line, with it the resume. Does nothing after a clean exit. For other launchers: `SnapRecomp.exe --play --resume`. |

A `SNAP_INPUT_SCRIPT` already set by the caller is kept and the resume is skipped (one log line). No launcher row exists for `crash_prompt` yet; edit config.json.

Log lines: `launcher: crash resume offered: ... scene=<s> target=<t> crashed=<0|1> save_flagged=<0|1>`, then `launcher: resuming to <target>`.

## Periodic checkpoints: not built

The game's own save only changes when the game commits it, and each commit is already atomic and keeps the older generation, so a checkpoint of the same files would copy what is on disk. A crash loses only what the game had not committed, and a quiet checkpoint cannot add what the game never wrote. The launch backup (above) covers the one case a checkpoint would help, a damaged save. Photo exports are written at their own safe points and are not copied twice. So there is no periodic checkpoint and no setting for one.

## Tests

- `crashresume.*` in `tests/hle/hle_tests.cpp` (`tests/hle/crash_resume_tests.h`): marker text, targets and scripts, `save_flagged` (clean save, kill mid-commit, torn `CURRENT`, stale pointer, nothing loadable).
- `tests/hle/cli_flags_test.cpp`: `--resume`, `--crash-prompt`.
- `tests/hle/crash_resume_test.py <exe>`: the built launcher with planted markers (no game starts): each last screen, a running owner, the three off switches, `--resume`, the script it writes, the marker after an answer.
- Ride proof: `SNAP_CRASH_TEST=kill SNAP_CRASH_TEST_AT=ride:<presents>` ends the process with `TerminateProcess` that many presents after a ride starts (no handler, no cleanup, like a power cut); `av`, `abort`, `gpu` and `terminate` take the same `SNAP_CRASH_TEST_AT=ride:<n>`; `quit` takes the clean exit path and must leave no marker. The run leaves `session.marker`; the next launcher start reports it and the save folder is unchanged.

Code: `host/crash_resume.h` (marker text, targets, script), `host/mods/crashlog.cpp` (`snap_session_*`, test hook), `host/mods/routes.cpp` (scene watch), `runtime/hle/fs.cpp` (clean exit), `runtime/hle/save_backup.cpp` (`save_flagged`), `host/launcher.cpp` (question), `host/cli_flags.h`.
