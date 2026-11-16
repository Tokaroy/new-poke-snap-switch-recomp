# Player options

Date: 2026-11-15. Five options for players: game language, problem report, A/B swap, Discord Rich Presence, and a note on text speed. Each row below says where it lives, its settings key, its environment variable and what it does. An environment variable set by hand beats the saved setting.

| Option | Launcher | F1 menu | Settings key | Environment variable | Default |
|---|---|---|---|---|---|
| Game language | Gameplay page | Display tab, Language | `game_language` | `SNAP_GAME_LANGUAGE` | `auto` |
| Problem report | Gameplay page, "Create problem report..." | Display tab, "Create problem report" | none | `SNAP_REPORT_DIR` (folder, default Desktop) | n/a |
| Check install | Gameplay page, "Check install..." | none | none | none | n/a |
| Swap A and B | Controls page | Display tab, Controller | `swap_ab` | `SNAP_SWAP_AB` | off (follows `input.json`) |
| Discord Rich Presence | dormant: no row unless an application id is set | dormant: no row unless an application id is set | `discord_presence` | `SNAP_DISCORD_PRESENCE` | off |
| Save state / Load state | none | Save state tab (buttons), F5 save, F8 load | none | none (tests: `SNAP_CHECKPOINT`, see `docs/save-states.md`) | one in-memory slot, this run only |

## Game language

Values: `auto`, `en`, `ja`, `fr`, `de`, `it`, `es`, `ko`, `zh-hans`, `zh-hant`. Needs a restart: the game reads its language once at start.

These are the languages the game ships text for (ten `TextResource_*` packs under `common/Text/master`). The row never offers a language the game does not have. `auto` maps the Windows display language to the closest one and falls back to English. Chinese picks Traditional for `zh-Hant`, `zh-TW`, `zh-HK` and `zh-MO`, Simplified otherwise.

The game's own voice language is a separate row in the game's Options screen (English or Japanese). This row changes text only.

The F1 menu itself follows this row and switches at once when you pick a language (the game's own text still needs the restart). Its text is `runtime/ui/ui_i18n_table.cpp`: one row per English text with French, German, Spanish, Italian, Japanese, Korean and both Chinese scripts; a text with no row shows in English. The game's UI font has Hangul and most kana but only part of the accented Latin letters and the kanji, so the menu merges Windows system fonts for the rest (Segoe UI, Yu Gothic, Malgun Gothic, Microsoft YaHei, Microsoft JhengHei); nothing is shipped, and a missing system font shows boxes for the characters it would have drawn. `python -I tests/hle/ui_i18n_check.py` checks the table; `tests/hle/ui_f1_lang_script.txt` shows every page in Japanese and German and ends with `expect_i18n`.

## Problem report

One zip, made on the player's PC, saved to the Desktop as `SnapRecomp-problem-report-<date>-<time>.zip` (or into `SNAP_REPORT_DIR`). It is never uploaded. The player attaches it to a bug report.

Inside: `logs/` (the program's log files, last 3 MB each), `settings/` (settings, input, audio, window and mod list files), `shadercache/` (stats, a file listing, and the pipeline key list when under 4 MB), `system.txt` (Windows version, CPU, memory, graphics cards with driver version, displays, `SNAP_*` variables) and a `README.txt` listing every file.

Left out: game files, keys, save files, photos, and the driver's pipeline cache blob.

Scrubbing: every text file has the Windows user name, the computer name, the account's display name and any `<drive>:\Users\<name>` folder replaced (with `<user>`, `<pc>` and `%USERPROFILE%`) before it is zipped.

## Check install

The Gameplay page has a "Check install..." button that compares your files with lists of known-good hashes and tells you in plain words what is wrong. Files are only read, no hash is shown, and nothing is uploaded.

Each part gets one of four verdicts: modified or damaged, from the wrong game version (it names the version it matches, for example 1.2.0), missing, or a mixed update (the main program file and the RomFS come from two different versions). The first line is a summary. Below it is one line per part.

The launcher checks the RomFS folder against the 2.0.1 check files and file-list hash, and the exe, the DLLs, the converted game images and the program files used for the build against `install_record.json`, which the builder writes next to the exe. The same text goes into the problem report as `install_check.txt`.

The command line check goes further: `python builder/snap_builder.py check --install-check --main <main> --sdk <sdk> --romfs <folder> [--install-dir <folder with the exe>]`. Beyond the launcher's checks, the main and sdk files are compared with all five known versions (1.0.0, 1.1.0, 1.2.0, 2.0.0, 2.0.1) using `builder/install_hashes.json`. Each part is optional, and the exit code is 0 only when nothing is wrong. A build made before this check has no `install_record.json`; run the command with `--write-record` once to record what is there now, after the check passed.

## Swap A and B

Swaps confirm and back on the controller only. The keyboard and mouse bindings do not change. The game's button pictures follow the swap. The base value is `controller.swap_ab` in `input.json`; the `swap_ab` setting or `SNAP_SWAP_AB` (`1` or `0`) overrides it live, no restart.

## Discord Rich Presence

Dormant (owner's call, 2026-11-16: no Discord application id will be registered). The code stays, but with no application id nothing of it is visible or active: the launcher and the F1 menu show no Discord row, the presence thread is never started, and the game never opens Discord's pipe. Setting `SNAP_DISCORD_APP_ID` (or the `discord_app_id` setting, or `kDefaultAppId` in `runtime/hle/discord_app.h`) wakes it for a developer; the rows then appear. The rest of this section describes that woken state.

Off by default. When on, the game talks to the Discord app on the same PC over Discord's local pipe (`\\.\pipe\discord-ipc-0` to `-9`), using our own small client. No Discord SDK or other binary is used, and nothing else is contacted. The activity holds the course name, a mode line and the time the session started.

The course is found from the course pack the game opens (`LevelComposite/master/LC_<id>_*.drp`); the name is read from the player's own extracted English text, so no game text ships with the project.

Discord needs an application id (a public number from the developer portal). Set it with `SNAP_DISCORD_APP_ID` or the `discord_app_id` setting; until one is set the feature is dormant as described above.

## Text speed and menu cursor speed

Not added. The game's own Options screen has no text speed row, and none of our existing hooks sees the dialogue reveal rate or the menu repeat rate (the interface-pace and skip hooks only skip waits). A speed row needs a new hook in the game's UI code; revisit when someone has traced one.
