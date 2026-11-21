# Aim feel

Options for the camera stick, the mouse and the gyro. Every default gives the behavior from before these options existed.

## How the game reads input (measured 2026-11-20, `SNAP_INPUT_STATS=1`)

The input hub polls the devices when the game calls `GetNpadState` (just in time), at most once per millisecond. At 120 fps (route `florio_kbm`, about 100 presented frames a second on a busy PC) the game made 2.0 pad reads per presented frame and the hub polled once per frame: the game reads per guest frame, not per 60 Hz tick. The sample the game gets is 0.08 ms old on average (0.8 to 1.0 ms at worst, when a second read inside the same millisecond was served from the previous poll). The first read of a frame comes 10 to 18 ms after the present call returns, so the stick value is sampled late in the frame.

What adds delay after the sample:

- Mouse look turns pixels moved into a velocity over a 40 ms window (plus the Smoothing slider, up to 340 ms): about 20 ms of average lag. The `mouse_window_ms` option shortens it.
- The cursor path (`GetCursorPos`, then re-center) can lose the movement that happens between the two calls and applies pointer acceleration. `raw_mouse` reads `WM_INPUT` instead and sums every event between two reads.
- The gyro gave the game the single newest sensor sample. `gyro_avg` samples about every millisecond on a thread and gives the game the mean since its previous read.

## Options

| Setting key | Env switch | Values | Default |
| --- | --- | --- | --- |
| `stick_curve` | `SNAP_STICK_CURVE` | `linear`, `classic` (t^1.5), `precise` (t^2.2) | `linear` |
| `stick_deadzone` | `SNAP_STICK_DEADZONE` | 0 to 0.6, or -1 to follow the controls menu | -1 |
| `gyro_smooth` | `SNAP_GYRO_SMOOTH` | 0 (off) to 1 (60 ms time constant) | 0 |
| `gyro_dead` | `SNAP_GYRO_DEAD` | deg/s, 0 to 10 | 1.5 |
| `gyro_avg` | `SNAP_GYRO_AVG` | 0 or 1 | 0 |
| `raw_mouse` | `SNAP_RAW_MOUSE` | 0 or 1 | 0 |
| `mouse_window_ms` | `SNAP_MOUSE_WINDOW_MS` | 8 to 80 | 40 |

A curve acts on the analog stick after the dead zone is removed; keyboard, mouse and gyro are not curved. All keys work with `--set key=value`, live in the F1 menu (Controls, Aim feel and Gyro aim) and as rows in the launcher's Controls page. An environment variable beats the setting.

`SNAP_INPUT_STATS=1` logs reads per frame, sample age and mouse totals every 5 seconds.

Photo and aim scoring read the stick values the game already gets, so scoring is unchanged: nothing here touches the game's own aim logic.
