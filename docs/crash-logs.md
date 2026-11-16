# Logs and crash reports

Date: 2026-11-15. Code: `host/mods/crashlog.cpp`, called from `host/launcher.cpp`, `host/boot.cpp`, `host/loader.cpp` and `runtime/nvn/nvn_vk.cpp`. Quirks in effect and the GPU line come from `docs/driver-quirks.md`.

## Files

The player's logs live in `%LOCALAPPDATA%\SnapRecomp\logs\`:

| File | What |
|---|---|
| `stderr.log` | this run's log |
| `trace.log` | this run's boot trace |
| `stderr.log.prev`, `trace.log.prev` | the run before (one generation; replaced at the next start). A log another process still holds open is left where it is. |
| `_crash.txt` | written on a fatal error; stays until the next fatal error replaces it |

The Options > Problem report zip (`host/mods/problem_report.cpp`) should carry `logs/stderr.log.prev`, `logs/trace.log.prev` and `logs/_crash.txt` next to the two current logs.

Test runs (`host\run_boot.ps1`) redirect stderr to `boot_stderr.log` in the run folder; `_crash.txt` then goes into that folder. `SNAP_CRASH_DIR=<folder>` overrides the folder in every case.

## What `_crash.txt` holds

- Time, process and thread, uptime in seconds.
- What happened: a host exception (code and name), `abort()`, an uncaught C++ exception, a fatal GPU error (`VK_ERROR_DEVICE_LOST` or `SURFACE_LOST` from acquire or present) or a fatal load error (game files unusable).
- The faulting address: module and offset (`snap_boot.exe+0x...`, or the driver DLL), plus the accessed address for an access violation. For the other kinds, the host stack as module+offset. For a host exception, the guest thread type, registers x0, x1, sp and the symbolized x30.
- Build: exe name, size, write time, PE timestamp and an FNV-1a 64 hash of the exe (the hash is computed on the final write only). `SNAP_BUILD_REV`, when a build defines it, is added.
- OS (Windows version and build, logical processors, memory), GPU (name, vendor and device ID, driver version) and the driver quirks in effect.
- The last 60 lines of the log.

A host exception writes the file twice: once at once (so a hang later in the crash handler still leaves a report) and again after the stack was printed, so the last log lines include the stack. Only the first crashing thread writes. Nothing on this path allocates.

Game code compiled from the guest has no unwind data, so Windows cannot walk the stack of a thread that faults inside it and ends the process without ever calling the host's crash filter (found while testing the forced crash: no report at all). A vectored handler installed at start catches that case: it walks the stack, and if no frame could handle the exception, it runs the host filter itself, which prints the stack and writes `_crash.txt`. A fault that some frame can handle is left alone.

## Test aid

`SNAP_CRASH_TEST=av|abort|gpu|terminate` forces that failure at present 120 (`SNAP_CRASH_TEST_AT=<n>` changes it). Off unless set.
