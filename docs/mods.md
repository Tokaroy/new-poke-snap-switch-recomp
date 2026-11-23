# Mods: loose-file DRP entry replacement

A mod replaces single entries inside the game's DRP archives (textures, models, FlatBuffers registries, Lua, UI, text) by
dropping the decompressed file into a pack folder. No archive is repacked. Everything is local: the repo holds the loader,
never game data or mod payloads made from it.

## Folder layout

Mods root: `%APPDATA%\SnapRecomp\mods\`, or the folder named by the `SNAP_MODS_DIR` environment variable.

```
mods\
  mods.json                      priority order and enabled flags (optional)
  my-hd-pack\
    pack.json                    name, version, author, description
    common\UI\master\Foo.drp\    <- the archive path, exactly as under rom:/ (the folder name ends in .drp)
      icon_a.nutexb              <- one decompressed entry, named like the extracted file
    romfs\                       <- optional: whole files laid over rom:/ by path
  ui-tweaks.zip                  a pack can also be a .zip (see "Zip packs")
  .unpacked\                     the loader's copies of enabled zips; safe to delete
```

The path below the pack folder is `<archive path>/<entry file>`, the same layout as
`extracted\drp-v262144\` (written by `scripts\drp_unpack.py`). The first folder whose name ends in `.drp` is the
archive; the file inside it is the entry. Files elsewhere in a pack (`pack.json`, readme, screenshots) are ignored.

How an entry is matched (case-insensitive):

- **Archive**: the path up to and including the `.drp` folder.
- **Name**: the file name without its last extension (`icon_a`). A name the game stores with its extension
  (`foo.nutexb`) is matched either way.
- **Type**: the extension, mapped to the game's extension id (crc32 of the lowercase extension; `nutexb` -> `5C156DBC`).
  An 8-digit hex extension (the unpacker's name for types it does not know, e.g. `DC4A8177`) is used as the id directly.

Limit: the unpacker names an entry whose name repeats inside one archive `name__<index>.ext`. The loader cannot tell such
entries apart, so that file overrides every entry with the same name and type in that archive.

## pack.json

```json
{ "name": "Magenta UI test", "version": "1.0.0", "author": "you", "description": "What it changes." }
```

All fields are optional text. The folder name (or the zip name without `.zip`) is the pack id used in `mods.json`, in the
launcher and in the log.

## Zip packs

A pack can be a single `<id>.zip` in the mods root instead of a folder. Inside the zip, `pack.json` sits either at the top or
in one top folder (a zipped pack folder works as is). Stored and deflate zips are read, zip64 included.

- The zip is never changed. When the pack is on, the loader unpacks it once to `mods\.unpacked\<id>\` and serves files
  from there. A `<id>.stamp` file next to the copy holds the zip's size and time; when the zip changes, the copy is made
  again at the next start. Deleting `.unpacked` is always safe.
- A file whose path would land outside the pack (`../`, a drive letter, an absolute path) makes the loader refuse the
  whole zip and turn the pack off for that start, with a warning in the log.
- A pack that is off is not unpacked. Its `pack.json` is still read so the launcher can show its name.
- A folder and a zip with the same id are one pack; the folder is used.

## mods.json and priority

```json
{ "packs": [
    { "id": "my-hd-pack", "enabled": true },
    { "id": "ui-tweaks",  "enabled": false },
    "older-pack"
] }
```

- The list is in priority order: the first pack wins when two packs replace the same entry. The loser is named in the log
  (`[mods] conflict: ...`).
- A plain string means enabled. A pack that is listed but missing, or on disk but not listed, is skipped (a warning is
  logged for the first case).
- Without a `mods.json`, no pack is on except `native-options` (the PC tabs of the options menu), which is on unless
  `mods.json` lists it as disabled.
- Changes apply at the next game start. There is no hot reload. Nothing in the game files is modified, so turning a pack
  off is the whole rollback.

## Conflicts

Two enabled packs conflict when both replace the same DRP entry, or both carry the same `romfs/` path. The pack higher in
the list wins. The loader logs each one at startup:

```
[mods] conflict: common/UI/master/Foo.drp/icon_a.nutexb: my-hd-pack beats ui-tweaks
```

The launcher's Mods page lists the same conflicts (it runs the same loader code): every path two enabled packs both
change, and which pack wins. Move a pack up to make it win.

### Seeing and changing the order

One loader, one `mods.json`: every way below reads and writes the same file and shows the same conflicts.

- **Launcher, Mods page.** The packs in priority order, with the arrows (or Q and E) beside each one to move it, and a count of shared
  files per pack. **Compare conflicts** (or X) opens **Which pack wins**: every shared file, textures first. Pick one to see both
  packs' versions side by side (the winner on the left, the hidden one on the right). A `.nutexb` is shown as a thumbnail (BC1 to BC5,
  BC7 and RGBA8, any size; the picture is read from your own files, only for the screen, nothing is saved or sent); other files show
  their sizes. **Enter** puts the losing pack just above the winner and saves `mods.json`. A zip pack is unpacked into
  `.unpacked` on the first preview, as the game would at its start.
- **In game, F1 menu, Mods tab.** The same order, arrows and on/off boxes, plus a Conflicts list (pairs of packs and how many files
  they share, and every file under "Every file"). Order changes need a restart.
- **Command line, no window** (the game does not start; `--data-dir` and other player flags go before it):

  ```
  SnapRecomp.exe --mods list                      order, on/off, names
  SnapRecomp.exe --mods conflicts [--files] [id]  pairs and counts; --files lists every file and the winner; id limits to one pack
  SnapRecomp.exe --mods order a b c               put these packs on top in this order (the rest follow)
  SnapRecomp.exe --mods move id up|down|top|bottom|<position>
  SnapRecomp.exe --mods enable id [id ...]        (and disable)
  ```

  Exit code 0 is done, 1 a failed write, 2 a bad argument. Packs that are on disk but not in `mods.json` are listed as off and are
  written into it on the first change. `native-options` and entries whose folder is missing are kept as they are.
- **Overlap report for a whole mods folder.** `python -I scripts\mods\overlap_report.py [--identical]` counts, for every pair of
  enabled packs, the files both change and which wins, and writes `analysis\modorder\overlap.md` (read only).

All HD packs (`hd-auto`, `hd-florio`, `hd-pokemon`, `hd_pikachu`, HD shadows and the rest) are ordinary packs: they load through
`mods.json` like any other, so the same order and conflict list cover them. The only texture path outside `mods.json` is the shiny
twin folder (`SNAP_SHINY_DIR`, `docs/shiny-mod-plan.md`), which the game reads separately and which is not ordered here.

## The HD texture pack

The builder's AI texture upscaler (`builder\HD-TEXTURES.md`) writes an ordinary folder pack, `hd-ai-textures`, and adds
it to the end of `mods.json`. So any pack you list above it wins where both change a texture, and the conflict list shows
where that happens. The pack is made from your own game files; do not share it.

## Making an override

1. Find the entry under `extracted\drp-v262144\` (the manifest `analysis\romfs\drp_unpacked.tsv` lists every entry).
2. Make your replacement in the same format as the original. For a texture, a `nutexb` with the same BC/ASTC format, or
   any new size you like (below).
3. Place it:

   ```
   python -I scripts\mods\make_override.py extracted\drp-v262144\common\UI\master\Foo.drp\icon_a.nutexb my_icon.nutexb --pack my-hd-pack
   ```

   This writes `%APPDATA%\SnapRecomp\mods\my-hd-pack\common\UI\master\Foo.drp\icon_a.nutexb` (and a starter `pack.json`).
   `--mods-root` and `--dry-run` are available.
4. Start the game and read the log (stderr, or the trace of `host\run_boot.ps1`).

Log lines:

```
[mods] root C:\Users\...\SnapRecomp\mods: 1 overrides
[mods] common/ui/master/foo.drp/icon_a <- my-hd-pack            (every override, once, at startup)
[mods] hit common/ui/master/foo.drp/icon_a <- my-hd-pack (4096 bytes)   (first load of each entry)
[mods] summary: 1 replacements served, 0 failed, 1 overrides installed  (at exit)
```

## Larger or smaller replacements

The replacement may be any size. The loader's output buffer is allocated from the entry's size field after the hook runs,
and the game then checks the data against a CRC32 that the hook recomputes, so an HD texture 4x the original just
allocates 4x the memory. Whether the rest of the game copes is a content question: a `nutexb` carries its own width,
height and mip sizes, so a self-consistent bigger texture works. Data whose size is fixed elsewhere (tables that other
code indexes by the original length) must keep its size.

## Read-ahead

A big HD pack makes the first load of a course wait on the disk: the loader reads every replaced texture from the pack folder
when the game first asks for it (the log's `[mods] hit` lines), up to thousands of files for one course. Read-ahead reads those
files into the Windows file cache a little earlier, on a low-priority thread, so the loader's own read is a copy from memory.
It never changes what the game receives and the loader never waits for it.

- Setting `mods_readahead` (`0` or `1`), variable `SNAP_MODS_READAHEAD`, flag `--mods-readahead on|off`. On by default.
  Needs no per-course list: the replacements of a pack file are found from the pack index.
- When the game opens a pack file (`.drp`) that has replacements, those files are queued. The course packs
  (`LevelComposite/master/LC_<id>_<x>.drp`) are grouped by course: opening another course cancels what is still unread of
  the old one, queued and running.
- Before the player picks a course, the course-select scene's pack file opens. At that point the most recent ride course
  of this player is read ahead (the same variant, for example day or night), from `.readahead.txt` in the mods folder: up to four
  pack file names, written by the game as courses open. A wrong guess costs reads and is cancelled by the pack the game really
  opens. `SNAP_MODS_READAHEAD_GUESS=0` turns the guess off.
- The menus' pack files are read ahead too: `.readahead.txt` also lists (as `u <name>` lines) the pack files with replacements
  that the game opened in its first two minutes, at most 100, so the next start reads them while the title screen loads.
  Delete the file to forget both lists.
- Bounded: no more than `SNAP_MODS_READAHEAD_MB` (default 1536) of files are read ahead and not yet loaded by the game, two
  reader threads (`SNAP_MODS_READAHEAD_THREADS`, 1 to 8), thread priority lowest. Files the game loaded already are skipped.
- The log carries `[mods] readahead on|off: ...` (loader read wait in total, files in the cache when the loader got to them,
  still reading, not started) and one line per pack file with its first and last load times. It is also printed every 3 seconds
  while it changes, because test runs end without a clean exit. The off figures are measured too, so on and off compare.

Measured with the Florio ride (`host/routes/florio_eval.txt`), the 99 packs it loads copied with unbuffered I/O so the file
cache starts cold, hd-auto 4 GB, hidden and muted, NVMe: five runs off and two on (final design); hard disk: two of each, in alternating order (2026-11-23):

| | read-ahead off | read-ahead on |
|---|---|---|
| NVMe: loader wait for the course pack (405 files) | 2.5 to 3.9 s | 0.47 to 0.67 s |
| NVMe: loader wait over the whole run | 25 to 53 s | 5.6 s |
| NVMe: reads over 50 ms | 6 to 101 | 0 |
| hard disk: loader wait for the course pack | 14.3 s, 16.9 s | 0.85 s, 0.54 s |
| hard disk: course pack opened to ride playable | 22.9 s, 22.4 s | 19.2 s, 16.1 s |
| hard disk: loader wait over the whole run | 205 s, 138 s | 141 s, 115 s |

On NVMe the frames over 25 ms and the time to the ride did not change beyond run-to-run noise (the waits are on loader
threads, not the render thread). The wait that remains is the copy out of memory plus the CRC check of each file, and menu
packs opened for the first time in a session.

## How it works (for developers)

- `recomp/snap_gen.py` `MAIN_INLINE_HOOKS` makes the generated code call `rt_mod_entry(Cpu*)` just before `main+0x5e3a58`,
  inside the DRP entry worker (`main+0x5e38b0`, the only caller of `OodleLZ_Decompress`). Regenerating `main` is needed
  once after pulling this change (`recomp/snap_gen.py --module main`, then `recomp/snap_build.py`).
- The hook (`runtime/mods/mods.cpp`) finds the archive and entry name, looks them up in a hash set built at startup
  (`runtime/mods/mods_core.cpp`), and on a hit points the worker at a host-built chunk: the original header with csize =
  dsize = file size, the file's CRC32, and the file as an uncompressed payload. With no packs installed the hook is one
  atomic load and a return.
- Zips are read by `runtime/hle/zip_read.cpp` (no third-party code; the save importer uses it too).
- Tests: `tests\mods\run_tests.ps1` (path mapping, index priority, size override, zip packs, conflicts) and
  `python -I tests\mods\test_make_override.py`. The same C++ test covers the `--mods` command (`mods::cli`) and the thumbnail
  reader (`runtime/mods/mods_thumb.cpp`).

## Code hooks

Packs replace data only. Host code that runs before or after a named game function (for example to log UI page changes or read the ride camera) is a separate, off-by-default mechanism with its own manifest, `<mods root>\hooks.json`: see `docs/mod-hooks.md`.

## Ideas from RT64 texture packs (not built yet)

RT64's `TEXTURE-PACKS.md` (MIT) describes N64 texture packs. Its packs are keyed by texture hash because N64 textures
have no names; ours are keyed by DRP path, which is stable and readable, so we keep paths. One thing we already match:
RT64 ships BC7 DDS with a full mip chain, and our builder writes BC7 `nutexb` with mips. Two ideas worth taking later:

- A low-mip cache: one small file holding the lowest mips of every replacement, loaded at startup, so a texture shows
  at low detail at once while the full one loads.
- zstd inside the zip: RT64 packs (`.rtz`) are zips compressed with zstd by default. Our zip reader takes stored and
  deflate only; zstd would make big packs smaller and faster to unpack.
