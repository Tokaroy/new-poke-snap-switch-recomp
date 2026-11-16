# Driver quirk table

Date: 2026-11-15. A small table in `runtime/nvn/nvn_quirks.{h,cpp}`, keyed by GPU vendor, device ID range and driver version range. A matching entry turns off one risky Vulkan feature or path on that GPU. The log says which entries applied, and every entry has an override.

## What can be turned off

| Feature name | What it is | Its own switch |
|---|---|---|
| `provoking_vertex` | `VK_EXT_provoking_vertex`: flat varyings come from the last vertex, as on the Switch GPU | `SNAP_VK_PROVOKING=0` |
| `custom_border_color` | `VK_EXT_custom_border_color`: arbitrary RGBA sampler borders | `SNAP_VK_BORDER=0` |
| `subgroup_pin` | pins shaders that use subgroup operations to 32 lanes (`subgroupSizeControl`) | `SNAP_VK_SGPIN=0` |
| `present_wait` | `VK_KHR_present_id` + `present_wait`, the queue depth cap in the frame pacer | `SNAP_PACE_WAIT=0` |

The renderer asks `snap_quirks::Off("<feature>")` right where it would enable the feature (`nvn_vk.cpp` device creation, `nvn_pace.cpp`). Without a match nothing changes.

## Overrides

| Variable | Effect |
|---|---|
| `SNAP_QUIRK=<feature>[,<feature>...]` | Force those features off on any GPU. Unknown names are logged and ignored. This is the way to test a quirk by hand or to try a fix for a bug report. |
| `SNAP_QUIRK_<ENTRY>=0` | Never apply that table entry (`<ENTRY>` is the entry's name in upper case). |
| `SNAP_QUIRK_<ENTRY>=1` | Apply that entry on any GPU. |
| `SNAP_QUIRKS=0` | Ignore the whole table. `SNAP_QUIRK` still works. |

All are read once, at device creation. An environment variable set by hand beats everything.

## What the log shows

One line at every start, with or without a match:

```
[quirk] GPU NVIDIA GeForce RTX 4090, vendor 0x10de (NVIDIA) device 0x2684, driver 621.14 (raw 0x...): 0 of 0 table entries applied
```

A matching entry adds `[quirk] applied <name>: turns off <features> (seen: <where>)` before it. A forced feature adds `[quirk] SNAP_QUIRK: <feature> off`, and the summary line ends with `; features off: ...`. The same text goes into the problem report (`logs/stderr.log`) and into `_crash.txt` (see `docs/crash-logs.md`).

## The table today: empty, on purpose

Entries need evidence: a real log, run or note where that GPU and driver misbehaved. None exists yet. Every run so far was on one NVIDIA RTX 4090, and no log or note shows a driver or GPU bug; the AMD and Intel paths are untested (the owner has no AMD card), and AMD-only fixes stay on hold until a tester shows a real problem. So the machinery ships with an empty table and the four switches above.

To add an entry, edit `kTable` in `runtime/nvn/nvn_quirks.cpp`:

```cpp
{"name", vendor, dev_lo, dev_hi, drv_lo_major, drv_lo_minor, drv_hi_major, drv_hi_minor, {"feature", nullptr}, "run id or report that showed it"},
```

- `vendor` 0 means any vendor; `dev_lo` and `dev_hi` both 0 mean any device; a `drv_hi_major` of 0 leaves the driver range open at the top. Both ends are inclusive.
- Drivers are compared as major.minor, decoded per vendor: NVIDIA `621.14`, Intel on Windows `101.5762`, everyone else the standard Vulkan packing.
- Put the evidence in the last field. An entry without one does not go in.

## Test

`tests/quirks/quirks_test.cpp` checks the matcher (vendor, device range, driver range, both ends, open end, per-vendor decoding). It builds with `-DSNAP_QUIRKS_TEST_TABLE` to exercise one entry end to end:

```
clang++ -std=c++20 -I runtime/nvn tests/quirks/quirks_test.cpp runtime/nvn/nvn_quirks.cpp -o quirks_test.exe
```
