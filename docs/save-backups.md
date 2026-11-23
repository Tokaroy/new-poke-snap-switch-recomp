# Save backups

Every start copies your save before the game opens it, and every save import or restore copies it first. The copies sit next
to the save and the live save is never deleted or edited by this.

## Where

For a profile, the save is `<profiles>\<id>\save` and the backups are `<profiles>\<id>\backup\`. With no profile the folder is
`<save folder>-backup`. Each backup is a folder named `YYYYMMDD-HHMMSS-<reason>`:

| Reason | Made | Kept |
| --- | --- | --- |
| `launch` | each time the game starts | the newest 10 |
| `import` | before a save import (launcher or command line) | the newest 10, counted apart from `launch` |
| `restore` | before a restore | the newest 10, counted with `import` |

Counting the safety copies apart means ten starts never push out the copy taken before an import. A start is skipped when the
newest backup already holds the same save generations, so launching a game you did not play does not add a copy.

A backup holds one consistent generation per user (the same copy the profile and export code make), so it is itself a valid
save folder.

## Settings

| Setting | Effect |
| --- | --- |
| `save_backup_keep` in the HLE config (`hle.json`, or `config.json` next to a release exe) | how many of each kind to keep, default 10; 0 turns backups off |
| `SNAP_SAVE_BACKUP_KEEP=<n>` | the same, from the environment (wins over the config file) |
| `SNAP_SAVE_BACKUP=1` | back up even when `SNAP_SAVE_DIR` is set (test runs skip it otherwise) |

The start log has one line: `save backup: <name> (N kept, newest K per kind)`, `save backup: nothing new to copy`, or
`save backup: FAILED ... (the game starts anyway)`. A failed backup never stops the game.

## Restore

Launcher, Saves card, **Restore a backup...** lists the newest eight with date, time and reason. Picking one asks to confirm,
backs up the current save as a `restore` copy, then writes the chosen backup back as a new generation. The generation that was
current stays on disk as an older generation, so the restore itself can be undone. A running game holds the save folder and
the restore is refused with a message; close the game first.

In the game, F1 > Saves > **Restore a backup** shows the same list for the running profile. The running game holds the save folder, so the pick is queued (`restore.pending` in the backup folder) and applied at the next start, before the game opens the save; **Restart now** does that at once. The queued request can be cancelled from the same list.

Code: `runtime/hle/save_backup.{h,cpp}`. Tests: `savebackup.*` in `tests/hle/hle_tests.cpp`
(`tests/hle/save_backup_tests.h`).

Whole-profile backups to a folder of your choice (a OneDrive or Dropbox folder, say), with a manifest, verification and restore into a new profile, are a separate feature: [sync-backups.md](sync-backups.md).
