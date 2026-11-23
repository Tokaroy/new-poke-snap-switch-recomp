# Profile backups to a sync folder

One zip of your profile, written into a folder you pick. Point it at a OneDrive, Dropbox or other sync folder and the copies leave this PC as well. This is separate from the [save backups](save-backups.md) that sit beside the save: those are quick local safety copies, these are portable archives.

## Set it up

Launcher, Settings, **Backups** page:

| Row | Meaning |
| --- | --- |
| Back up my profile | On makes a backup each time the game closes normally after your save changed. Off by default. |
| Backup folder... | The folder the zips go in. Choosing one offers to turn backups on. |
| Copies to keep | Zips kept per profile: 1, 2, 3 (default), 5, 10 or 20. |
| Back up saves / settings / photos | What goes in. All three by default. |
| Back up now... | Makes a backup right now, even when nothing changed. |
| Restore a backup... | Lists the zips with their date, size and a summary of what is in them. |

HD texture packs and shader caches are never included: they can be rebuilt.

The choices are kept in `backup.json` in the data folder (next to `window.json`). Every row has a command-line flag; a flag wins for one run and is never written back:

| Flag | Environment variable | Meaning |
| --- | --- | --- |
| `--backup on\|off` | `SNAP_SYNC_BACKUP` | Back up when the game closes. |
| `--backup-folder <folder>` | `SNAP_SYNC_BACKUP_FOLDER` | Where the zips go. |
| `--backup-keep <n>` | `SNAP_SYNC_BACKUP_KEEP` | Copies kept per profile, 1 to 100. |
| `--backup-include <list>` | `SNAP_SYNC_BACKUP_INCLUDE` | Any of `saves`, `settings`, `photos`. |

`--backups` is the action form (no window, the game does not start): `--backups` or `--backups list` lists the zips, `--backups now` makes one, `--backups verify [<number>]` re-reads one against its manifest, `--backups restore <number|latest>` puts one into a new profile, and `--backups restore <number|latest> --into-current --yes` replaces the current profile's save. See [command-line.md](command-line.md).

## When a backup is made

- After the game closes normally, when backups are on and the save changed since the newest zip of that profile. The game starts a windowless background copy of the exe (`--backups auto`, BelowNormal priority) that waits for the game to exit first, so it never reads a save the game is writing. A crash makes no backup.
- When you press **Back up now** (or run `--backups now`).

A backup is skipped, with a warning shown under **Back up now** and in `backup_last.txt`, when the save:

- is in use (a running game holds its `.snap.lock`),
- is mid-write (a generation without its completion marker, or an interrupted `CURRENT.tmp` replace),
- is flagged for recovery (a missing or torn `CURRENT`, an orphaned generation, a restore queued for the next start), or the profile has a clear or delete queued.

Start the game once and close it, and the next backup goes through.

## The zip

`<folder>\SnapRecomp-<profile id>-<YYYYMMDD-HHMMSS>.zip`, stored without compression (photos and saves are already dense), ZIP64 whenever a file or the archive passes 4 GB. Inside:

| Entry | Content |
| --- | --- |
| `save/<uid>/CURRENT`, `save/<uid>/gens/<n>.ok`, `save/<uid>/gens/<n>/...` | The one consistent generation of each user. |
| `settings/profile/settings.cfg`, `settings/global/window.json`, `audio.json`, `input.json` | The profile's settings and the shared window, audio and controls files. |
| `photos/photos/...`, `photos/screenshots/...` | The photo folder (the `photo_dir` setting, else `<data>\photos`) and the screenshots folder. |
| `manifest.json` | Written last: every file with its size and SHA-256, the save generations, what was included, a one-line summary. |

How it is written:

1. The new zip is written to `<name>.zip.part` in the target folder. A sync client that sees it sees a temporary name.
2. The `.part` is re-read against its manifest (size, SHA-256 and the zip's own CRC-32 of every file, nothing missing, nothing extra). A bad one is deleted and reported; nothing older is touched.
3. Only then it is renamed to `.zip`.
4. Only after that, the oldest zips beyond **Copies to keep** are deleted. A damaged zip is never counted as a copy and never deleted by the rotation.
5. A leftover `.part` from an interrupted run is deleted at the start of the next run and is never listed.

The free space of the folder is checked first.

## Restore

**Restore a backup...** lists the zips newest first. The default target is a NEW profile named `<profile> (backup YYYY-MM-DD)`: the save is imported as a new generation, the profile's `settings.cfg` is put in, and photos that are missing are copied back (an existing photo is never overwritten). The window, audio and controls files are not restored, because they are shared by every profile. The zip is verified first; if it does not verify, or any step fails, the new profile is removed again and the current profile is untouched.

Replacing the current profile's save is a second choice and asks twice. It takes a local `restore` backup of the current save first, writes the zip's save as a new generation (the one that was current stays as an older generation), and is refused while a game holds the save folder.

## Code and tests

`runtime/hle/sync_backup.{h,cpp}` (zip writer, manifest, verification, rotation, restore), `host/launcher.cpp` (Backups page actions, `--backups`, the run after the game), `host/launcher_ui.cpp` (the page). Tests: `syncbackup.*` in `tests/hle/hle_tests.cpp` (`tests/hle/sync_backup_tests.h`): SHA-256 vectors and settings, the zip and manifest, ZIP64 paths (the threshold is lowered so small files take them), rotation, interrupted writes, saves that are in use, mid-write or need recovery, restore into a new profile and over the current one. They use temp folders only.
