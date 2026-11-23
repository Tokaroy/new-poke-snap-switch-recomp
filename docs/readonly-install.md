# An exe in a folder you cannot write to

Added 2026-11-23. The game keeps its build, saves, settings, caches and logs next to its exe ([install-folder.md](install-folder.md)). In `C:\Program Files`, a read-only share, or a folder that Windows' Controlled Folder Access protects, that is impossible. The game now notices at the start and tells you, instead of failing later or writing somewhere you did not expect.

## How it notices

At every start of a release build (a `config.json` or `portable.txt` sits next to the exe) the exe makes a folder and a file in its own folder, writes a few bytes, flushes, and deletes both. It does not guess from the folder's name or permissions. A refused write counts, whatever the permissions say, which covers Controlled Folder Access. A probe file that cannot be deleted again also counts as a failure. The probe leaves nothing behind. Development builds without those two files are not checked.

Nothing is asked when:

- `--data-dir <folder>` is given: you chose the data folder, and the game reports a folder it cannot write to (exit code 2).
- The exe was started by a game that already settled this (`SNAP_PORTABLE=1`).
- The folder is writable.

## What you see

A plain window titled "This folder is read-only for the game". It names the folder, says what failed, says that nothing has been changed, and offers two choices (or Cancel, which ends the start with exit code 1):

1. **Move to a folder I pick.** A folder picker opens at `%LOCALAPPDATA%\SnapRecomp\Game`. The game copies its whole install into a new `SnapRecomp` folder inside the one you pick, checks every file and folder (names and sizes, and the exe byte for byte), and starts the copy with the same arguments. The old folder is not touched: the window tells you it can be deleted once the copy works, and nothing in the game deletes it. A stopped copy stays in `SnapRecomp.copying` for you to delete. The copy is refused, with the old folder unchanged, when the target is inside the old folder, already holds a `SnapRecomp` folder, is not writable, or has too little room.
2. **Use a data folder elsewhere.** A folder picker opens at `%LOCALAPPDATA%\SnapRecomp\Data`. The game keeps running from the read-only exe, but saves, settings, mods, caches and logs go to the folder you pick, in the same layout as `--data-dir` (`Roaming` and `Local` inside it). The choice is remembered for this exe path in `%LOCALAPPDATA%\SnapRecomp\data_dirs.txt`, one line per exe, so the next start asks nothing. When the remembered folder is gone or no longer writable, the question comes back.

The install root is what gets copied when `config.json` says the exe sits in `<root>\<exe_rel>`, which is how setup lays it out; the next start of the copy then rewrites every stored path under the old root, the same as a moved folder ([install-folder.md](install-folder.md#moving-the-folder)). A game finished by a very old builder (no `install_root`) copies only the exe's own folder, and its stored paths keep pointing at the old folder, so do not delete that one.

## For other launchers and scripts

No window opens when `--play`, `--no-prompt`, a launcher action (`--update`, `--check-install`, ...) or one of the other commands (`--import-save`, `--benchmark`, ...) is on the command line. If the folder is not writable and `--data-dir` is missing, the exe prints the reason to stderr and exits with code 3:

```
SnapRecomp: this folder is read-only for the game (a folder could not be created (Access is denied.)): C:\Program Files\Snap Recomp\work\out
The game keeps its build, saves and settings next to the exe. Start it with --data-dir <folder> ...
```

A folder remembered through the window (choice 2) is used silently in these runs too. `--data-dir <folder>` always works and is the choice for a launcher to pass.

## Where the code is

- `host/readonly_logic.h`: the write probe, the pointer file text, the decision (`resolve`), and the copy and check, with no Windows includes. Tested in `tests/hle/readonly_tests.h`, including a folder made unwritable with an ACL deny.
- `host/readonly_install.cpp`: the window, the pickers, the copy thread, the pointer file in the real `%LOCALAPPDATA%`, the restart. Called first in `main()` (`host/boot.cpp`).
- `host/portable.cpp` (`snap_portable_init`) stays the one place that turns a data folder into `APPDATA` and `LOCALAPPDATA`. The remembered folder goes through it exactly as `--data-dir` does, and sets `SNAP_DATA_DIR` for the builder and child processes.
- The one-exe setup program (`setup\setup_main.cpp`) already falls back to `%LOCALAPPDATA%\SnapRecomp\setup` when its own folder cannot be written. It was not changed.

Test switches: `SNAP_RO_AUTOCHOICE=move:<folder>` or `data:<folder>` answers the window without showing it; `SNAP_RO_NO_START=1` makes the move stop after the check.
