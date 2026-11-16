# Controls and remapping

Added 2026-11-16. Every input the game reads can be rebound on the keyboard, the mouse and a controller, from the F1 menu. The launcher shows a read-only summary.

## What can be rebound

The game reads a Switch pad, so a game action is a Switch input: A, B, X, Y, L, R, ZL, ZR, Plus, Minus, the four D-pad directions, the two stick clicks, and the four directions of each stick (movement and camera aim). That is 24 inputs. Each one has two slots for each device (Keyboard 1 and 2, Mouse 1 and 2, Controller 1 and 2). A mouse slot takes a button, X1, X2 or a wheel notch; a controller slot takes a button, a trigger or a stick direction.

## F1 > Bindings

- Click a slot, press the key, button or axis. Esc cancels, Delete or right-click clears.
- A source used by more than one input is marked with `!` and the color of the slot. Two lines under the table name the shared sources and the inputs that use them (`P: A, B`); hovering a marked slot lists the rest. A conflict is allowed: both inputs fire together.
- Reset buttons: one per device (keyboard, mouse, controller), plus "Reset all controls to defaults". Each asks first.
- Changes apply live and are saved at once to `input.json` (below). The page also shows the exact file path.

## Where the binds are stored

`%APPDATA%\SnapRecomp\input.json` (`SNAP_CONFIG_DIR` overrides the folder), format version 2, the same file as the mouse, controller and hold-to-toggle options (`docs/hle-services.md`, `Mapping::to_json`). It is one file for the whole install, not per profile. A key that is missing keeps its default, so a file can hold only the binds that changed, for example `{"version":2,"buttons":{"A":["pad:east","key:P"]}}`. An invalid file is kept as `input.json.invalid`, the defaults are used and a notice shows in the game.

## Launcher

The launcher's Controls page ends with a read-only summary: one row for each of A, B, X, Y, L, R, ZL, ZR, Plus and Minus with all the sources bound to it (keyboard, mouse, controller), then the stick keys. It reads the same `input.json`. The rows cannot be edited there; the last row says to use F1 > Bindings in the game.

## Defaults for the keyboard and mouse (2026-11-16)

WASD move, arrows aim, L or Enter A (shutter), Space ZR (shutter), K or 1 B (fruit), J or 2 Y (orb), I or 3 X (scan), Q / E L / R, Left Shift or the mouse wheel ZL (zoom), P or Tab Plus, Backspace Minus, T F H G the D-pad. Esc is B in menus and Plus (pause) on a ride. Mouse look is on by default: on a ride the mouse aims, the left button is the shutter and the right button zooms; in every menu and screen the cursor is free and a click is the touch screen. Each action keeps two keyboard slots; the extra keys are in the Bindings tab and can be cleared.

## Switching to another app

Alt+Tab, Win+Shift+S and a third-party snipping tool take the keyboard focus, and the keys you press for them (Tab, Shift, and the Esc that closes a snipping tool) can still be down when the game window comes back. Esc is Plus on a ride and Tab is Plus everywhere, so the pause menu used to open. The input hub (`runtime/hle/input_core.cpp`, `Hub::poll`, fed by `Hub::set_window_focus` from `host/window_opts.cpp`) now:

- ignores every key and mouse button while the window is in the background;
- after the focus is back, ignores each key and button that is already down until it is released (a key you pressed again counts as new);
- ignores Esc and the left mouse button for 300 ms after the focus is back, so the Esc that closes a snipping tool and the click that brings the window back do not reach the game.

Gamepads are not affected. `SNAP_FOCUS_GUARD=0` turns the guard off (for tests); `SNAP_FOCUS_GUARD_LOG=1` logs every Plus and B press the game is about to see.

## Switching to another app

Alt+Tab, Win+Shift+S and a third-party snipping tool take the keyboard focus, and the keys you press for them (Tab, Shift, and the Esc that closes a snipping tool) can still be down when the game window comes back. Esc is Plus on a ride and Tab is Plus everywhere, so the pause menu used to open. The input hub (`runtime/hle/input_core.cpp`, `Hub::poll`, fed by `Hub::set_window_focus` from `host/window_opts.cpp`) now:

- ignores every key and mouse button while the window is in the background;
- after the focus is back, ignores each key and button that is already down until it is released (a key you pressed again counts as new);
- ignores Esc and the left mouse button for 300 ms after the focus is back, so the Esc that closes a snipping tool and the click that brings the window back do not reach the game.

Gamepads are not affected. `SNAP_FOCUS_GUARD=0` turns the guard off (for tests); `SNAP_FOCUS_GUARD_LOG=1` logs every Plus and B press the game is about to see.

## Button prompts

The Xbox and PlayStation pictures of the button-icons setting already follow the controller binds: the picture shown for a Switch input is the first controller source bound to it, and it changes when you rebind (`runtime/hle/btn_icons.cpp`, `glyph_of_action`, fed by the same mapping through `icons::set_mapping`). Keyboard binds have no prompt pictures: the game draws its own Switch pictures for a keyboard player. That needs new art and is not done.

Message text ("Press X to run a scan") is drawn from the font, not from a picture. Under the Xbox style the font's Switch letters A, B, X and Y are swapped for the letters of the pad buttons that do the action, so in the default position layout the game's "X" reads Y (`btn_icons.cpp` `text_glyph_code`, called by the `button-icons` hook on `nn::fontll` `AcquireGlyphmap` and `CheckGlyphExist`, `host/mods/modhooks.cpp`). The game caches a glyph after it is first drawn, so a style or binding change shows in text after a restart. `SNAP_BTN_TEXT=0` turns it off. Proof: `tests\hle\run_btntext.ps1` (route `btntext_scan`, tutorial fixture), with `-Off` for the before capture.

## Tests

- `tests\hle\run_focus_test.ps1` (hidden runs, 4 of them): the menu and a Florio ride, each with the guard off (the "before": the held Esc and Tab press B and Plus when the window comes back, and on a ride open the pause menu) and on (nothing presses, then a real Tab press still works). `SNAP_WINDOW_TEST` replays the leaving: `wait:ride>=25,blur@3,keydown:escape@4,keydown:tab@5,focus@6,keyup:escape@7,keyup:tab@8` (host/README.md, "Window options"). The unit test is `focus_guard` in `tests\hle\hle_tests.cpp`.
- `tests\hle\run_focus_test.ps1` (hidden runs, 4 of them): the menu and a Florio ride, each with the guard off (the "before": the held Esc and Tab press B and Plus when the window comes back, and on a ride open the pause menu) and on (nothing presses, then a real Tab press still works). `SNAP_WINDOW_TEST` replays the leaving: `wait:ride>=25,blur@3,keydown:escape@4,keydown:tab@5,focus@6,keyup:escape@7,keyup:tab@8` (host/README.md, "Window options"). The unit test is `focus_guard` in `tests\hle\hle_tests.cpp`.
- `tests\hle\run_binds_ui.ps1` (hidden run, 15 checks): F1 > Bindings, rebind A to P, rebind B slot 2 to P (a conflict), check both binds are kept, "reset all", check the defaults. It dumps frames to `analysis\ui\binds_dump`.
- `tests\hle\run_rebind_proof.ps1`: a hidden run of `host/routes/rebind_shutter.txt` with an `input.json` that binds A to `pad:east` and `key:P` only. The route rides Florio, holds the old shutter key L (the film counter must stay at 72 for 200 presents), then P (the counter drops: a photo was taken).
- The route token `src=<kind:name>` (`runtime/hle/input_core.cpp`, host/README.md "Controls menu (F1)") holds a host source (`key:P`, `mouse:left`, `pad:east`) through the binding table. Every other route token presses a Switch button after the mapping, so it cannot test a rebind.
- The test script command `reset_binds <all|kb|mouse|pad>` runs the reset buttons without the question (`SNAP_UI_TEST`).
