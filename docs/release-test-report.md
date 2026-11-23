# Release test report

One command writes a local folder of evidence for a build: unit tests, a ride with fps and load times, A/B frames, launcher pictures.

```powershell
powershell -NoProfile -File scripts\release\test_report.ps1 -BuildDir build\snap            # full run
powershell -NoProfile -File scripts\release\test_report.ps1 -BuildDir build\snap -Quick     # unit tests + the florio_eval ride
```

Options: `-Out <folder>` (default `build\release_report\<stamp>`), `-NoAB`, `-NoLauncher`, `-StepTimeout <s>` (default 600).
Exit code: 0 everything passed (or was a known failure), 1 a step failed, 2 bad arguments (or an `-Out` inside the checkout that git does not ignore).

## The report stays local

The folder holds game frames (ride, score screen, A/B, launcher with the user's game state). Do not commit it, upload it or attach it to a
release. The default folder is under `build\`, which git ignores, and the script refuses an `-Out` inside the checkout that is not ignored.
`report.md` and `report.json` are scrubbed of user profile paths, the account name, the machine name and the checkout path; the logs in
`logs\` are scrubbed the same way. Nothing game-derived is in the script or its helpers.

## What runs, in order, one at a time

Every child runs at BelowNormal. Compile steps take the main checkout's `build\heavy.lock` one step at a time; a step over `-StepTimeout`
(10 minutes) is killed by PID and fails. While `build\play.lock` is live (the owner is playing) the rides, A/B captures and launcher
pictures are skipped and the report says so.

| Step | Source | Full | Quick |
|---|---|---|---|
| HLE tests (known failure allowed: `padfx`) | `tests\hle\run_tests.ps1` | yes | yes |
| CRC32 test | `tests\hle\run_crc_test.ps1` | yes | yes |
| CLI flags test | `tests\hle\run_cli_flags_test.ps1` | yes | yes |
| Mods tests | `tests\mods\run_tests.ps1` | yes | yes |
| ELF check, picture rows tests | `tests\hle\run_elf_check_test.ps1`, `run_picture_rows_test.ps1` | yes | no |
| Ride `florio_eval` (to the professor's score) | `host\run_boot.ps1` with `SNAP_PERF` | yes | yes |
| Ride `bench_florio` (built-in benchmark route) | same | yes | no |
| A/B, default vs Enhanced lighting, two fixed points (Florio rail 40, Blushing Beach sea) | `host\ab_capture.ps1` | yes | no |
| Launcher pictures (1920x1080, every page) and a contact sheet | `scripts\ui\capture_launcher.ps1` | yes | no |

The HLE allowance is `$AllowedHle` at the top of the unit-test section; the report lists a used allowance. Any other failing test fails the step.

### Rides

- fps: average, 1% low and 0.1% low over the ride window (`florio_eval`: the `ride` wait to the `film<=0` wait; `bench_florio`: `ride` to `rail>=110`),
  from the game's `SNAP_PERF` per-present times (the same log `host\bench.ps1` uses, same definition of low: fps of the mean of the slowest 1% / 0.1%).
- Load times: every `wait ... met after <s> s` line in the run's `boot_stderr.log` is one transition (title to hub, map to ride start, course end to
  photo select, evaluation to score). They are game time between screens and include the route's own key presses, so they are upper bounds on the load itself.
- Score screen: the script rides a temporary copy of the route with three short frame bursts after the score screen opens, copies the frames into `images\`, and
  reads numbers off them with the Windows built-in OCR (`scripts\release\ocr_image.ps1`). The numbers are an OCR read of the pictures, not a value from the
  game: check them against the frames. The step fails when the route does not end (`end of route reached`), exits non-zero, times out or never reaches
  the score screen.
- Pass means: exit code 0, route end reached, no wait timeout.

### Shader cache seed (fresh checkouts and worktrees)

A fresh checkout has an empty translated-shader cache (`<BuildDir>\nvn_cache`, and the run slot's copy). Rides then show black frames and time out at
`map_select`. Before the first ride the script copies the main checkout's `build\runs\slot0\nvn_cache` into any empty target with `robocopy /E /XO`
and records that in the report notes. If no warm copy exists, the note says so and the ride is likely to fail. A worktree also needs `build\fixtures` as a
junction to the main checkout's (`build\eh`, `analysis`, `extracted`, `work` and `tools\*` too); the script does not create them.

## Output

```
<Out>\report.md        pass/fail table first, build and machine info, rides, A/B, launcher, what is not covered
<Out>\report.json      the same results, small
<Out>\images\          score screen frames
<Out>\ab\              A/B contact images and json
<Out>\launcher\        launcher pictures and a contact sheet
<Out>\perf\            SNAP_PERF frame logs of the rides
<Out>\logs\            one stdout / stderr file per step, scrubbed
```

Machine info: GPU and driver, CPU, RAM, OS build. No user name, computer name or user path.

## Release flow

`builder\release_package.py --test-report <BuildDir> [--test-report-full]` runs the report after the zip passes the leak scan and keeps it in
`build\release\test_report\v<version>`. It is optional and never stops the release or goes into the zip; read the report before publishing.

## Not covered

- Photo-scoring differential (`tests\photoscore`): it needs a capture recorded in game and a readable rewrite kept outside the repository. Run it by hand
  before a release that touches scoring (`docs\photo-scoring.md`).
- Courses other than Florio and Blushing Beach (A/B only), save states, HD texture packs, DLSS and controller paths. Use the testing budget in `AGENTS.md`.
- The report does not compare against an earlier report; compare two `report.json` files by hand.
