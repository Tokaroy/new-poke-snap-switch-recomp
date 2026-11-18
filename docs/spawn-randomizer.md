# Spawn randomizer

Shuffles which Pokemon appear at the spawn points of each course, from a seed you type. It is off by default and changes
no game file: the generator writes a mod pack of changed `LV_MAP_*.levelb` entries (the course placement files) into your
mods folder, and the loader serves them in place of the originals (see [mods.md](mods.md)). Switch it off and you have the
original game back.

The pack is built from your own extracted game files, so it never goes into git. The repo holds only the generator,
`scripts/mods/make_spawn_randomizer.py`.

## Use it

1. F1 menu, Photo tab, "Spawn randomizer": pick **Within course** or **All courses**, type a seed in "Randomizer seed" (any
   text; empty means "snap"), then "Apply and restart".
2. The same seed and mode give the same layouts on every PC, so you can share a seed.
3. The pack is called "spawn-randomizer" in the launcher's Mods page, where you can also switch it off or reorder it.

The pack is built when the game starts through `host\play.ps1` (step 1c, only when mode or seed changed) and again at
"Apply and restart" when `play.ps1` started the game. From a bare `SnapRecomp.exe` there is no Python to run, so build the
pack by hand first:

```
tools\venv-recomp\Scripts\python.exe -I scripts\mods\make_spawn_randomizer.py --seed hello --mode within
tools\venv-recomp\Scripts\python.exe -I scripts\mods\make_spawn_randomizer.py --mode off          (remove the pack)
tools\venv-recomp\Scripts\python.exe -I scripts\mods\make_spawn_randomizer.py --if-stale         (read the F1 settings)
```

Other options: `--out <mods root>` (default `%APPDATA%\SnapRecomp\mods` or `SNAP_MODS_DIR`), `--data <unpacked DRP root>`,
`--courses <regex>`, `--report` (print what stays fixed, write nothing), `--selftest`. `--if-stale` reads `spawn_rand` and
`spawn_seed` from the settings file (or the environment variables `SNAP_SPAWN_RAND`, `SNAP_SPAWN_SEED`).

The generator reads the unpacked game data (`extracted\drp-v262144`, made by `scripts\drp_unpack.py`) because the DRP
entries are compressed. The same holds for the other generated packs.

## Modes

| Mode | What changes | Status |
|---|---|---|
| Within course | A course keeps its own Pokemon; they swap places among that course's spawn points. The change is the 4-byte link from each spawn record to a Pokemon template, so the file keeps its size. | Tested: Florio Nature Park (Day), seed "hello", full ride to the photo score (2026-11-17). Other courses not ridden. |
| All courses | Each course's Pokemon templates are re-skinned to Pokemon taken from other courses, then shuffled as above. | Generator self-test only (the changed files parse). Not ridden yet. |

Neither mode has been tested on every course. If a course stalls, switch the randomizer off and report the seed.

## What stays fixed

A spawn that a script, the rail or a story event depends on must not move, or a course could stop making sense or stall.
The generator keeps these where the game put them (it never edits the camera rail, its event records, paths or any
number besides the Pokemon link):

| Rule | Kept fixed |
|---|---|
| prop | Everything that is not a Pokemon: scenery and event props (flowers, nuts, branches, rocks, the Bidoof dam parts, backgrounds, eggs, apples), NPCs and other special characters. Scripts and rail events name them. |
| form | Special forms and event variants (names like `CH_064_01_0` or `CH_034_01`). These are the story versions. |
| multi | A spawn that places several Pokemon together (a group with a leader). |
| param | A spawn with an extra script parameter (record slot 10): the timed or counted event spawns of a scene. |
| single | The only spawn of its species in a level. A Pokemon placed once is nearly always a secret, boss or event Pokemon. |
| shared | All mode only: a template shared with a fixed spawn cannot be re-skinned without changing that spawn. |

Pokemon that follow a path are shuffled only among the other path spawns, so a walker stays on a route. The full list for
your seed is written to `fixed.txt` next to the pack, one line per course with the rule counts and the fixed Pokemon.
`--report` prints it without writing the pack.

FIXED_LIST

## Photo scoring

Scoring reads the photographed Pokemon's own data (species, pose, size, position), so it works on whichever Pokemon stands
at a spawn. The routes below reach the professor's Score screen with randomized Pokemon.

PROOF

## Limits

- A new Pokemon in a place the game did not design for it can look odd (a swimmer on dry ground) and can behave in ways the
  original did not. That is the point of the mode, but it is not tested for every pair.
- The pack is built from the unpacked game data, so a player needs the same `extracted\drp-v262144` the other generators use.
- A changed seed needs a restart: the loader reads mod files once at start.
