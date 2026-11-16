# Skip course intro

At the start of every course the game plays a fixed intro: the NEO-ONE warp-in with an orbiting camera, about 10 seconds of game time. With this option on, the intro runs about 8 times faster and the ride starts after one or two seconds. Nothing is cut: the game's own timers, effects and camera run through the intro and finish it the normal way.

## Setting

| Where | Values | Default |
|---|---|---|
| `SNAP_SKIP_INTRO` | `1` on, unset or `0` off | off |
| Settings key `skip_course_intro` | `1` / `0` | `0` |
| Launcher > Gameplay: "Skip course intro" | Off, On | Off |
| F1 > Photos: "Skip course intro" | Off, On (restart) | Off |
| Game Settings > native PC tab "Interface": "Skip course intro" | On, Off (restart) | Off |

The option is read once when the game starts.

## How it works

Code: `host/mods/introskip.cpp`, `os_time_skew` in `runtime/os/os_kernel.cpp`, one call in `rt_gm_neo_speed` (`host/mods/neofilm.cpp`). No new inline hook, so `main` is not regenerated.

1. The course rail object R (the NEO-ONE's path, curve length 124.95 on Florio) is held still from its spawn by `R+0x1c = +inf` (a hold timer). The course state machine (`main+0x628850`, state 7) waits for an intro timer and then sends message `0x11e4ec2f`, which stores 0 there and starts the ride. The writer is `main+0x4beb50` (message handler, store at +0xb4e).
2. The existing rail-update hook (`main+0x4c1df4`) calls `snap_intro_rail(R)` once per rail update. While the hold is set and the rail progress is 0, each call moves guest time forward (`os_time_skew`): the next frame's delta is about 8 times the real frame time, at most 0.2 s. The game's clock accepts gaps under 0.5 s as the frame delta, so every timer, particle, animation and camera path sees a longer step and finishes sooner.
3. When the state machine releases the rail, the skew stops. The log shows `[intro] present=<n> intro done: <frames> frames, <s> s of game time skipped`.

Only game time moves. Frames are still simulated and drawn one by one, nothing in the game's state is written, and the rail, spawns and Pokémon are driven by the same code as without the option.

## Measured

Route `score_time_a` (Florio, `SNAP_NEO_SPEED=3`, hidden, 1080p), runs `0-20261115_232030` (off) and `2-20261115_232032` (on):

| | Off | On |
|---|---|---|
| Course start to ride (route "wait ride") | 20.7 s | 14.1 s |
| Intro frames | about 610 | 54 (7.3 s of game time skipped) |
| Reaches Score and End | yes | yes |

## Scores

Route `score_time_a` to the professor's Score screen, read from the dumped frames. The route's photos are not frame-identical between runs (the ride is steered by timed taps), so scores are compared per part and per subject, not photo by photo. Every run reached `evaluation:Score` and `End` (exit 0).

| Run | Skip | Dodrio: Pose / Size / Direction / Placement / Other / Total | Bidoof: Pose / Size / Direction / Placement / Other / Total |
|---|---|---|---|
| `3-20261116_001357` | off | 703 / 158 / 845 / 836 / 68 / 2,610 | not picked |
| `1-20261116_002617` | off | 500 / 152 / 845 / 815 / 39 / 2,351 | 500 / 35 / 714 / 712 / 0 / 1,961 |
| `2-20261115_233908` | on | 500 / 157 / 857 / 779 / 33 / (2,326, sum of the parts) | 500 / 52 / 716 / 688 / 30 / 1,986 |
| `2-20261116_002457` | on | 500 / 155 / 857 / 802 / 39 / (2,353, sum of the parts) | 500 / 48 / 716 / 693 / 0 / 1,957 |

The two off runs differ from each other as much as off differs from on (Dodrio 2,610 against 2,351; Pose 703 against 500 comes from which animation frame the shot caught). Size, Direction and Placement sit in the same bands in both modes, and the Bidoof totals are 1,961 off against 1,957 and 1,986 on. Direction 857 (both on runs) against 845 (both off runs) is a 12-point gap on a 1,000-point scale; earlier Dodrio photos from other work scored Direction 851 and 857 too, so this is the range of the pose that gets photographed. Four runs cannot settle a gap that small. Screenshots: `analysis/boot/route_score_time_a_<run>_NN.png`.

The ride itself is unchanged: the rail, the spawns and the Pokémon scripts are the same code, and the skip stops the moment the state machine releases the rail (`[intro] ... intro done` in `boot_stderr.log`).

## Other waits the same mechanism could cover

Not done; each needs its own hold condition found first:

- The rest of the ride after the last shot and the course-end lines before photo select (in `score_time_a` about 57 s pass between the last shot and the folder screen, most of it the ride itself).
- The professor's lines after the course start (`skip_cutscenes`, #18, already presses A for dialogue boxes).
- The "NOW LOADING" tip cards: they cover real loading time and are not a timer.

## Developer switches

- `SNAP_SKIP_INTRO_X=<2..30>`: the speed-up factor (default 8).
- `SNAP_INTRO_TRACE=1`: the rail's timer fields every 15 presents.
- `SNAP_INTRO_WATCH=1`: a hardware write watchpoint on R+0x1c; logs the rip and return addresses of each writer (resolve with the link map).
