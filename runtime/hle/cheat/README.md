# Standalone cheat VM

`cheat_vm.h/.cpp` parses text sections and executes the 19 documented Atmosphere opcode types through a caller-supplied memory interface. It uses C++17 and the standard library. New code uses GPL-3.0 with the additional linking permission in this directory's LICENSE.

The encoding source is [the upstream format documentation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/docs/features/cheats.md), read November 15, 2026. No implementation source or community cheat file was used. All tests use synthetic instruction words.

## Use

```cpp
cheat::Program program;
cheat::Error error;
if (!cheat::parse(text, program, error)) {
    // Report error.line and error.message.
    return;
}
cheat::Vm vm(std::move(program));
vm.enable(0, true); // ordinary cheats default to disabled; select the intended index
auto result = vm.run(memory_adapter, 100000);
// Report result.error if !result.ok; completed writes are not rolled back.
```

Provide all `Memory` methods, including checked byte reads/writes, four region bases, held-key bits, pause, and resume. Debug logging is an optional callback. The VM never dereferences guest addresses, opens files, or owns a background timer. Its byte interface is little-endian; the caller must bound and synchronize accesses to the emulated address space. Call `run` only at an appropriate memory/scheduler safe point. Serialize VM calls and do not run concurrently with enable/reset operations.

Braced sections are masters and execute first on every `run`, regardless of enable flag. Enabled bracketed sections follow in file order. General and saved registers reset at the beginning of each run and are shared across sections within that run; 128 static registers persist until `reset`. Replacing the program means constructing a new VM. Enabling/disabling a section resets its C4 edge latches.

The parser accepts eight-digit hexadecimal words separated by whitespace, CRLF/LF, an optional UTF-8 BOM, and blank lines. `#` and `//` comments outside headers are local extensions. Header text may contain those characters. Errors include source line numbers. Limits default to 16 MiB input, 1,048,576 words, 4,096 sections, and block depth 64. Instructions may span lines. The VM constructor revalidates instructions and rebuilds block targets rather than trusting public structure fields.

Every instruction is decoded before block pairing. Immediate words that look like an end instruction remain data. Nested conditional/else blocks and loops are matched structurally; crossed blocks, mismatched loop registers, duplicate else, truncation, invalid widths, and reserved/unknown opcodes are rejected.

## Opcode coverage

| Type | Implemented behavior |
|---|---|
| 0 | Immediate stores, widths 1/2/4/8, region plus register plus 40-bit offset |
| 1 | Six unsigned memory comparisons, direct/register offset variants |
| 2 | Matched end and else |
| 3 | Fixed-count loop start/end using the selected register |
| 4 | Full 64-bit immediate register load |
| 5 | All four documented memory-load address modes |
| 6 | 64-bit literal narrowed to write width, optional offset and increment |
| 7 | All five legacy integer operations |
| 8 | All-bits-held 28-bit key conditional |
| 9 | Ten integer/bitwise/move operations and four float operations; register/immediate operands; float widths 4 and 8 |
| A | All six store-address modes and optional increment |
| C0 | Six register-comparison operand modes and six unsigned comparisons |
| C1 | Save, restore, clear saved, clear working register |
| C2 | The same four actions through a 16-bit register mask |
| C3 | 128 persistent static registers, read/write halves |
| C4 | Full 64-bit key mask, one-shot/repeat modes |
| FF0 | Caller pause callback |
| FF1 | Caller resume callback |
| FFF | Five operand modes, width and log-ID callback |

Reserved B and other undocumented extended types are rejected. C-F/F0 are encoding prefixes, not independently executable operations. There are no intentionally missing documented opcode types; native compatibility remains unverified until the tests compile and run.

## Explicit semantic choices

The format page does not fully define VM state lifetime or all edge cases. These choices are implementation policy, not claims about undocumented upstream behavior:

- The task's held-key interface drives both key opcodes. A mask matches when all its bits are held. C4 mode 0 additionally requires a new match; mode 1 repeats. Latches update when the instruction executes, so skipped/disabled sections do not observe releases. The documentation mentions `hidKeysDown`; an adapter that supplies only edges would produce different behavior.
- General/saved registers reset per run; static registers persist. Masters execute first. These policies are stated above because the format page does not settle cross-section/tick state lifetime.
- A zero-iteration loop skips its body. Loop end decrements a positive register and repeats while its previous value exceeded one. A cheat that edits its loop counter can extend the loop, but the global step budget still applies.
- Integer results wrap at the selected width and zero-extend into registers. Comparisons are unsigned. Shifts greater than or equal to that width return zero. The documented logical AND/OR/NOT/XOR operations are implemented as bitwise operations. Move copies its left operand.
- Type 5 mode 2 uses the encoded S base register; the prose also mentions R, which conflicts with that encoding. Type A mode 5 uses R as its offset register because x selects the memory region. Type C1 clear operations act on destination D. These interpretations need interoperability fixtures if exact upstream equivalence is required.
- Address addition or access-range overflow fails before the affected memory callback. The failing run stops; earlier writes remain. An increment can fail after its associated write, so execution is not transactional.
- Pause/resume callbacks should be idempotent. A normal run ending after FF0 leaves the process paused until an explicit resume; the caller owns recovery/UI. A failing/budget-exhausted run attempts resume if that run paused the process and has not resumed it. A pause left by an earlier successful run remains the caller's responsibility.
- Floating arithmetic uses host float/double bit representations and the host floating-point environment. It does not emulate a separate floating-point control mode or sanitize NaNs/infinities.

The global budget counts executed instructions across masters and enabled cheats. Skipped blocks use validated targets without executing their memory operations. A budget error carries the current source line and executed-step count.

## Tests

`build.bat` uses `cl /std:c++17` or `g++ -std=c++17` and runs `test-output/cheat_tests.exe`. It returns 2 if no compiler is available. The test memory object is a bounded byte array; no game data or real addresses are used.

Synthetic cases exercise every documented opcode type, all arithmetic operation selectors at width 4, all memory widths/regions for static stores, nested conditions/else, fixed and zero loops, 64-bit key masks and C4 edges/repeat, master/enable behavior, saved/static registers, pause/resume, logging, malformed input, immediate data containing a terminator, address overflow, and budget cleanup. This is not exhaustive coverage of every addressing/width combination.

The coordinator reported 199 passing synthetic checks for the original module
using MSVC 14.51 with /W4. The byte fill now uses `uint8_t{0}` to remove its
reported conversion warnings. The expanded tests add every type-5, type-A,
C0, and FFF address mode, parser limits, and forged public instructions/targets.
Those new tests have not run here: the mailbox has no native compiler on PATH.
Before integration, run the expanded suite, add independent interoperability
fixtures for the policy choices above, and validate the scheduler/memory adapter
at a safe point. No game or menu wiring is included.
