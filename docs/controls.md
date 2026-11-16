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

## Button prompts

The Xbox and PlayStation pictures of the button-icons setting already follow the controller binds: the picture shown for a Switch input is the first controller source bound to it, and it changes when you rebind (`runtime/hle/btn_icons.cpp`, `glyph_of_action`, fed by the same mapping through `icons::set_mapping`). Keyboard binds have no prompt pictures: the game draws its own Switch pictures for a keyboard player. That needs new art and is not done.

## Tests

- `tests\hle\run_binds_ui.ps1` (hidden run, 15 checks): F1 > Bindings, rebind A to P, rebind B slot 2 to P (a conflict), check both binds are kept, "reset all", check the defaults. It dumps frames to `analysis\ui\binds_dump`.
- `tests\hle\run_rebind_proof.ps1`: a hidden run of `host/routes/rebind_shutter.txt` with an `input.json` that binds A to `pad:east` and `key:P` only. The route rides Florio, holds the old shutter key L (the film counter must stay at 72 for 200 presents), then P (the counter drops: a photo was taken).
- The route token `src=<kind:name>` (`runtime/hle/input_core.cpp`, host/README.md "Controls menu (F1)") holds a host source (`key:P`, `mouse:left`, `pad:east`) through the binding table. Every other route token presses a Switch button after the mapping, so it cannot test a rebind.
- The test script command `reset_binds <all|kb|mouse|pad>` runs the reset buttons without the question (`SNAP_UI_TEST`).
