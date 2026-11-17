# Local CI check (`host/ci_check.ps1`)

There is no remote CI. `host/ci_check.ps1` runs the fast checks by hand before a commit or a release. It exits 0 when every check passes. When one
fails it exits non-zero, and the last line says which check and why:

```
CI CHECK FAILED: identity: 1 finding(s), first [home-path] docs/foo.md: contains a user home path: ...
```

No git hooks are installed. Run it yourself, or let the release scripts run it (below).

```powershell
powershell -NoProfile -File host\ci_check.ps1                  # commit mode: the changed and new files
powershell -NoProfile -File host\ci_check.ps1 -Mode release    # every tracked file, plus the builder unit tests
powershell -NoProfile -File host\ci_check.ps1 -Only license,docs
powershell -NoProfile -File host\ci_check.ps1 -Skip hle,lifter
```

## Checks

| Name | What it checks |
| --- | --- |
| `license` | `LICENSE` and `README.md` agree: license title, the README badge, the License section (starts with the same name and links `LICENSE`), the copyright holder, and `THIRD_PARTY_NOTICES.md` exists. |
| `identity` | The leak scan. It reuses the rules of `builder/leak_scan.py` on the changed files (release mode: every tracked file): game-derived extensions and file names, keys and key-like blobs, the owner's names and addresses (`build/release/leak_terms.txt`, never committed), home-folder paths, e-mail addresses. It also reads author, committer and message of every unpushed commit. |
| `docs` | The unslop banned-phrase scan on the lines added to changed `.md` files. Hard flags fail the check, soft flags print a note. |
| `locks` | `host/tests/locks.Tests.ps1`. |
| `quirks` | `tests/quirks/quirks_test.cpp`, the driver quirk matcher. |
| `crc` | `tests/hle/run_crc_test.ps1`. |
| `hle` | `tests/hle/run_tests.ps1`: HLE, save, option and host unit tests. |
| `lifter` | A differential smoke of the AArch64-to-C lifter (`tests/snap_func.py`, see `docs/lifter-diff.md`): 120 functions, 4 states each, Unicorn against the lifted C. It fails on any mismatch or crash, and when fewer than 40 functions could be tested at all. |
| `builder` | Release mode only: `python -m unittest discover -s builder/tests`. |

`quirks`, `crc`, `hle` and `lifter` compile or run heavy work. They take `build/heavy.lock` (one heavy build at a time on the PC) and run at BelowNormal
priority. `-NoLock` skips the lock. Logs: `build/w/ci_check/<name>.log`.

### What the identity check allows

The scan is the release leak scan with four rules turned off, because they only make sense for a built package: the DLL manifest, license entries,
the product-name rule and game file names written as text (the runtime opens the game's archives by name, so source and docs name them). A game
file committed under its own name is still caught. Known, reviewed findings go in `host/ci_allow.txt` as `path-glob|rule  # why`. The release scan's
`builder/leak_allow_content.txt` pairs apply as well. With no `leak_terms.txt` the check fails instead of passing: a scan that cannot look for the
owner's names must not pass.

## Release scripts run it

`release_prebuilt.py`, `release_package.py` and `release_setup.py` call `builder/ci_gate.py` first. It runs `host/ci_check.ps1 -Mode release` and
stops the step (exit 4) when it fails. A pass is remembered in `build/release/ci_pass.json` for the exact tree: HEAD plus the content of every
changed file. The three steps of one release run the checks once. Any edit, a new commit or a pass older than two hours runs them again.

Override, when you must release past a failing check:

```powershell
python -I builder\release_package.py --skip-ci-check "locks test flakes on this PC, run by hand twice"
```

The reason is required. The script prints `!!! CI GATE OVERRIDDEN for <step>: "<reason>" ... !!!` and appends the time, step, commit and reason to
`build/release/ci_override.log`.
