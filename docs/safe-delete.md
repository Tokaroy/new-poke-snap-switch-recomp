# Deleting trees that may hold junctions

2026-11-16: a cleanup of `build\w\ct_tree` ran `Remove-Item -Recurse` on a scratch tree that contained junctions into the repository.
Windows PowerShell 5.1 (and Python's `shutil.rmtree` before 3.12, on older paths) enters a junction and deletes what it points to, so
`tools\` was emptied for the whole project.

## Rules

- Scripts: use `Remove-TreeSafe <path>` (`host\safe_rm.ps1`, loaded by `host\locks.ps1`, so every script that takes a lock has it). It unlinks
  every junction, symlink or other reparse point without entering it. It also takes a pipeline (`Get-ChildItem ... | Remove-TreeSafe`).
- Python (builder): use `snapbuild.safedel.rmtree` (`builder/snapbuild/safedel.py`), never `shutil.rmtree`.
- One link by hand: `cmd /c rmdir <link>` (no `/s`) or `[IO.Directory]::Delete(<link>, $false)`.
- Prefer copying to linking for scratch trees (`robocopy /MIR` of the few folders needed). If a tree must hold links, name the link target in the
  folder's notes and unlink before any other cleanup.
- Never run a recursive delete on a folder you did not create in this run.

## Guards

- `builder\tests\test_safedel.py`: junction and symlink targets survive (nested, root link, cycle back into the tree, read-only files), and the
  builder source holds no bare `shutil.rmtree`. Runs with the rest of `python -I -m unittest discover -s builder/tests`.
- `tests\safe_rm_test.ps1`: the same for `Remove-TreeSafe` with real junctions, plus a scan that `host\run_boot*.ps1`, `ab_capture.ps1`,
  `water_score_ck.ps1`, `play.ps1` and `locks.ps1` have no recursive `Remove-Item`.
