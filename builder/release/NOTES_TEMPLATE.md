<!--
Release notes template. Copy it with:  python builder/release_notes.py new --version 1.2.3 > notes.md
Fill every section, then check it:     python builder/release_notes.py lint notes.md
Keep the headings and their order; the updater shows this text in the launcher before anyone installs.
-->
## Known issues

- None known.
<!-- One bullet per problem a player can hit, with the workaround if there is one. Write "None known." when there are none. -->

## Saves and settings carry over

Updating keeps your saves and settings. Before anything changes, the updater copies them to a dated folder named update-backups next to the game's data and keeps the newest 3 copies. Going back to the previous version also makes a copy first. A portable install (portable.txt next to the exe) keeps everything in its own portable_data folder and carries it across the update.

## Driver notes

- NVIDIA: no known driver problems. Tested on driver version X.
<!-- Name the driver versions you tested and any version to avoid. Write "No driver notes." when there is nothing to say. -->

## Changes

- Short plain description of the change ([#123](https://github.com/finalpantasy/new-pokemon-snap-recomp/pull/123))
<!-- One bullet per change, each linked to its pull request or issue. -->

## Install or update

Run SnapRecompSetup.exe, or open the launcher and choose Updates. The Testing channel in the launcher's Updates page also offers early builds; Stable is the default.
