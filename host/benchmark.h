// SPDX-License-Identifier: MIT
// Built-in ride benchmark (host/benchmark.cpp). `SnapRecomp.exe --benchmark` (also the launcher's Benchmark row and the F1 menu's Benchmark tab)
// plays a fixed scripted Florio ride in a second copy of the game on a scratch save profile, reads the frame-time and per-pass GPU logs it
// wrote, and shows a results screen; the numbers are saved as JSON and a PNG in Documents\Snap Recomp\benchmarks.
//
//   --benchmark                  run it with the frame limiter off (SNAP_FPS_UNCAPPED=1) and vsync off, to see what the PC can do; extra arguments
//                                are passed on to the game, which is how a build without config.json is started
//   --benchmark-capped           keep the frame limiter at the frame rate target instead (the numbers then top out at the target)
//   --benchmark-view <file.json> show the results screen of a saved run (no game is started)
//   SNAP_BENCH_DIR=<folder>      where results are saved (default Documents\Snap Recomp\benchmarks)
//   SNAP_BENCH_AUTOCLOSE=<s>     test switch: close the results window after s seconds
#pragma once
#include <string>
#include <vector>

namespace snap_benchmark {

// True when the command line asks for the benchmark (--benchmark or --benchmark-view).
bool wanted(int argc, char** argv);

// Runs the whole thing from the command line: returns the process exit code (0 = results shown, 2 = the game could not be started or the
// ride did not finish). `args` are the arguments of this process without the program name; --benchmark* ones are consumed here.
int run_cli(const std::vector<std::string>& args);

// The launcher calls this from its own process (config.json is next to the exe, the game files were checked); same as run_cli but never
// exits. Returns true when results were produced.
bool run_from_launcher(bool uncapped);

// Makes the benchmark's scratch profile (packed unlock_all save, the player's settings copied) under `dir` for another tool that plays scripted
// routes (host/shaderprep.cpp). Returns the profiles folder (SNAP_PROFILES_DIR) and the profile id (SNAP_PROFILE); false with `err` on failure.
bool make_scratch_profile(const std::string& dir, std::string& profiles_root, std::string& profile_id, std::string& err);

}  // namespace snap_benchmark
