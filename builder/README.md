# Snap Recomp Builder for Windows

Works with your own copy of New Pokémon Snap 2.0.1.

This builder turns your own copy of the game into a native Windows program. It reads three things you supply, translates the game's code on your PC, compiles it, and links it with the prebuilt runtime that ships with this project. The result is `SnapRecomp.exe`.

The project contains no game data and does not help you get any. You need a legal copy of the game, version 2.0.1, and you extract the files from it yourself with your own tools.

Status: command-line only. A small window for picking the files comes later.

## What you supply

Extract with your own tools, from your own copy of game version 2.0.1 (the base game with the 2.0.1 update applied):

1. `main`, the program's main module file (an NSO).
2. `sdk`, the second module file from the same folder (an NSO).
3. The patched RomFS, extracted to a folder.

The builder accepts compressed or decompressed module files. It checks all three against the published 2.0.1 values before it does anything slow, and it stops with a plain message if something is wrong. Examples: a folder instead of a file, a file that is still encrypted, a module from an older version, a RomFS that has only the base game.

Nothing you supply is copied, uploaded or shipped. The builder reads your files and writes only inside the work folder you name.

## Requirements

| Need | Notes |
|---|---|
| Windows 10 or 11, 64-bit | The PC needs AVX2 (any CPU since about 2015). |
| Vulkan 1.3 graphics driver | To run the game. Needed for the smoke test at the end of the build. |
| LLVM 23 (clang and lld) | Pass its `bin` folder with `--llvm`. |
| Visual Studio Build Tools 2022 | The C++ tools and the Windows SDK. Free from Microsoft. The final link needs their system libraries. |
| Vulkan SDK | Provides `vulkan-1.lib`. Set `VULKAN_SDK` or pass `--vulkan-lib`. |
| Python 3.11 or newer | With `capstone`, `pyelftools`, `numpy` and `lz4`. Pass it with `--python`. |
| Function lists, or Ghidra | See "Function lists" below. |
| Disk and memory | About 3 GB of free disk for the work folder. 8 GB of RAM is the minimum for the build. The Ghidra step needs 16 GB or more (12 GB heap). |

Downloading and checking these tools automatically is planned. For now every tool is a local path.

## Use

Check your files first (takes about a second):

```
python builder/snap_builder.py check --main <main> --sdk <sdk> --romfs <romfs folder>
```

Build:

```
python builder/snap_builder.py build --main <main> --sdk <sdk> --romfs <romfs folder> --work <new empty folder> --maps <folder with the two function lists>
```

Useful options: `--jobs N` (parallel compiles), `--llvm`, `--python`, `--vulkan-lib`, `--prebuilt`, `--skip-smoke`, `--harness` (the shader harness DLL used by the smoke run), `--force <step>` (rerun one step).

The work folder holds everything the build makes: the converted modules, the function lists, the generated C, the compiled libraries, the log (`builder.log`) and `out/`. `out/SnapRecomp.exe` is the result, with its DLLs and a `config.json` that records the RomFS folder.

## Steps

Each step records what it used. If you run the builder again with the same work folder, finished steps are skipped, and an interrupted compile resumes where it stopped.

1. Preflight: Windows, CPU features, free disk, RAM, tools.
2. Version check: module build IDs, a hash of each decompressed module, and a RomFS file list check.
3. Convert the two modules to ELF files (`scripts/nso2elf.py`).
4. Function lists (see below).
5. Generate C for `main` and `sdk` (`recomp/snap_gen.py`, about half a minute).
6. Compile the generated C in parallel with clang.
7. Link the generated code, the prebuilt runtime objects in `prebuilt/` and the system libraries into `SnapRecomp.exe`.
8. Write `config.json`.
9. Smoke run: start the game on a scratch profile and wait for the title screen.

## Function lists

The code translation needs two lists of addresses, `functions_final.tsv` and `jump_tables.tsv`. They describe your game files, so they are made on your PC and never shipped.

- With `--maps <folder>` the builder uses lists you already have.
- Without it, the builder runs Ghidra 12.1.2 headless (with its Switch loader extension and JDK 21; pass `--ghidra` and `--jdk`). That takes about 13 minutes and a 12 GB heap. The `scripts/ghidra/` scripts do the work.

The Ghidra path is written but has not been run end to end yet.

## What is in the prebuilt folder

Our own runtime, host code, mods, the settings overlay and the upscaler glue, compiled to plain object files. No game bytes are in them. `builder/release_prebuilt.py` makes the folder, and `builder/leak_scan.py` checks it before release: no module or key files, no game file names, no personal names or home-folder paths, a size cap per file, and every DLL listed with its hash.

The builder and the prebuilt folder always come from the same release. A number in `runtime/cpu.h` (`SNAP_RUNTIME_ABI`) ties them together, and a mismatch stops the build.

## Tests

```
python -I -m unittest discover -s builder/tests
```

The tests use fake inputs only.

## License

MIT, like the rest of the project. Third-party notices are in `THIRD_PARTY_NOTICES.md`.
