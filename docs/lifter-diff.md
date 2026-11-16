# Lifter differential test (`tests/snap_func.py`, `tests/snap_sweep.py`)

Checks that the lifted C does what the original AArch64 code does, function by function. Each function gets random machine states (registers, SIMD,
flags, a scratch memory buffer). The same state runs in **Unicorn** (an AArch64 CPU emulator, the reference) and in the **compiled lifted C**; the
tool compares every register, the flags, the stack and the memory both sides wrote. A difference is a lifter bug (or a test-harness gap).
Idea borrowed from ACNHRecomp's per-function differential tests.

## License: dev only, never shipped

Unicorn is **GPL-2.0** (upstream `COPYING`; the PyPI wheel's metadata says "BSD", which is a packaging label, treat it as GPL-2.0). It lives only in
the private venv `tools\venv-recomp` (gitignored). It is not linked into the generator, the runtime, the exe or any release, and no installer or
build script may pull it in. `THIRD_PARTY_NOTICES.md` records this. Everything the tool writes (`analysis\`, `generated\`, `build\`) is gitignored,
and none of it is committed or distributed: it contains game code and game memory.

## Commands

```powershell
# one sample: ~500 functions chosen by category (atomic, jump table, FP/SIMD, leaf, calls, loops, NVN); about 9 minutes
$env:SNAP_DIFF_TAG = 's7'; tools\venv-recomp\Scripts\python -I tests\snap_func.py --seed 7 --functions 500 --cflags "-march=x86-64-v3 -O2"

# the whole set, in slices of 400, one process at a time, resumable; result in analysis\recomp\diff_sweep\
tools\venv-recomp\Scripts\python -I tests\snap_sweep.py [--size 400] [--slices A:B] [--cases 8] [--budget-min N] [--aggregate-only]

# re-run one minimized repro on both sides
$env:SNAP_DIFF_TAG = 'r'; tools\venv-recomp\Scripts\python -I tests\snap_func.py --cflags "-march=x86-64-v3 -O2" --replay analysis\recomp\diff_sweep\repro\0x5b520.json
```

`SNAP_DIFF_TAG=<name>` sends the output to `analysis\recomp\diff_<name>`, `generated\subset_<name>` and `build\subset_<name>`, so runs never overwrite each other.
The runs lower their own priority (BelowNormal). A slice deletes its emitted C and objects when it ends.

## Output

`analysis\recomp\diff_sweep\`:

| file | content |
|---|---|
| `summary.json` | slices done, status counts (pass / fail / crash / untested), per category, random states compared, time, why functions were untested |
| `results.tsv` | one row per function: entry offset, size, category, status, cases run, first difference |
| `mismatches.tsv`, `mismatches.md`, `mismatches.json` | only failing and crashing functions |
| `repro\<entry>.json` | minimal input: stack pointer, the few non-zero registers that still make the two sides differ, memory patches, the first differences, the replay command |
| `repro\<entry>.s`, `.c` | the AArch64 disassembly and the lifted C of that function |

Status: **pass** every accepted state matched; **fail** a state differs; **crash** the C side faulted; **untested** Unicorn could not run any random state
for that function (it ran off the scratch memory, hit an unsupported instruction, or never finished within `--steps`). The reason is in `results.tsv`.

## How it decides

* A function runs alone. Every call (`BL`/`BLR`) goes to one stub on both sides that mixes the arguments and the return address into a digest and returns a
  value derived from it, so the *calls a function makes and their arguments* are compared too. A function that calls another function in the same sample
  is run in a second executable where the callee is a stub.
* Memory: a scratch buffer (85% pointers into itself, 15% zero) and the `.bss` of the image are seeded identically on both sides. Unicorn repairs states
  that fault (swaps a bad base register for a scratch pointer, or patches an 8-byte slot, at most 16 patches); the repaired state is what the C side gets.
* Functions with syscalls, system registers, exclusive-load/store pairs outside the supported set and similar are skipped before running (`forbidden_mask`).
* **Minimizer**: for each failing function, registers, SIMD registers, flags, FPCR and patches are zeroed or dropped one at a time and the state is kept
  only if the two sides still differ. The repro is the smallest state found, usually 1 to 3 registers.
* **Self-test**: `--mutate N` corrupts one operation in N emitted functions; every corrupted function that can run must show up as a fail.
  Last run: 6 mutated, 5 detected, 1 crashed the C side (also counts as caught), 0 missed.

## Result of the first whole-set sweep (2026-11-15, seed 1, 8 states per function, `-O2`)

* 25,496 functions in the image. **11,201 were tested**: 10,505 pass, 696 untested, **0 mismatches**, 72,534 random states compared, 93 minutes (47 slices of 400).
* Not tested at all: 6,866 smaller than 6 or larger than 600 instructions; 5,391 that `check_function` rejects (tail call into the middle of another
  function, a `br` the jump-table resolver cannot read, no `ret`); 2,015 that pass those checks but are left out by the call-structure rules in
  `pick_functions` (recursion, or a caller of another dropped function: the stub trick covers one level); 23 with a forbidden instruction or a generator split.
* Untested (696): 102 hit the Unicorn step cap (loops that wait on memory the random state never changes), 78 the call cap, the rest ran off the mapped memory
  or hit an exception Unicorn does not model.
* Self-test (`--mutate`): the harness finds injected lifter faults and minimizes them (a corrupted `add` in a prologue came back as a 2-register repro).
  With no real mismatch to show, that is the proof the zero is not a blind spot of the tool.

## Limits

* Random states reach what random states reach. A branch that needs a specific register value, or a function that needs real game state, can stay
  untested or only half covered. Recorded inputs from a real run (hook `FN_ENTER`/`fnprof_hit`, store arguments and memory) are the next step if the
  untested share is too high; not built yet.
* Floating-point flag results and `FPCR` modes are compared, but a state where Unicorn and the host disagree on a NaN payload rounding would show
  as a fail to look at by hand.
* The sweep tests the lifted C compiled with `--cflags` (default `-march=x86-64-v3`, the sweep uses `-O2`). A difference that depends on other flags is not looked for.
