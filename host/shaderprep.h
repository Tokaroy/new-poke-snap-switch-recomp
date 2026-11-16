// SPDX-License-Identifier: MIT
// Shader preparation (host/shaderprep.cpp). `SnapRecomp.exe --prepare-shaders` plays one scripted ride per course in a hidden second copy of
// the game, on a throw-away save profile, so the pipeline service (runtime/nvn/nvn_pipeline.cpp) records every graphics pipeline the game uses
// into THIS PC's shader cache and compiles it. Afterwards the first visit to a course finds its pipelines already built. Nothing is shipped:
// the key list, the driver cache and the progress file are made on the player's PC, in <data dir>\shadercache.
//
//   --prepare-shaders                 all courses, hidden, at below-normal priority (about 20 to 30 minutes)
//   --prepare-shaders --courses a,b   only these course keys (see --prepare-shaders --list)
//   --prepare-shaders --force         run again even when the cache is already marked prepared
//   --prepare-shaders --visible       show the game window of each ride (debugging)
//   --prepare-shaders --list          print the course keys and exit
//   --prepare-shaders --stop          ask a running preparation to stop after the current ride
//   SNAP_PREP_SPEED=<n>               ride speed multiplier (default 3, SNAP_NEO_SPEED)
//   SNAP_PREP_TIMEOUT=<s>             time limit of one course (default 900)
// Extra arguments are passed on to the game (a development build without config.json is started that way, e.g. --main ... run).
//
// <shadercache>\prepare.json (written after every course; the launcher's chip reads it):
//   {"format":1,"state":"running|done|partial|stopped|failed","index":3,"total":14,"course":"Blushing Beach","ok":2,"failed":0,"started":<unix>,
//    "updated":<unix>,"pipelines":<keys known>,"seconds":<elapsed>,"results":[{"key":"florio","exit":0,"seconds":88,"new_pipelines":812}, ...]}
// "done" is only written when every course ran ("partial" after a --courses subset, shown as stopped); the pipeline service deletes the file when the cache is wiped for another build or translator.
#pragma once
#include <string>
#include <vector>

namespace snap_shaderprep {

bool wanted(int argc, char** argv);
int run_cli(const std::vector<std::string>& args);

struct Progress {
  int state = 0;            // 0 no file, 1 running, 2 done, 3 stopped, 4 failed
  int index = 0, total = 0; // course being played (1-based) and number of courses
  int ok = 0, failed = 0;
  std::string course;
  unsigned pipelines = 0;   // keys known (prepare.json), status.json "ready" when present
  unsigned ready = 0;
  long long updated = 0;    // unix time of the last write: a "running" file older than 20 minutes is treated as stopped
};
// Reads <cache_dir>\prepare.json and status.json; false when there is no prepare.json.
bool read_progress(const std::string& cache_dir, Progress& p);
// Starts `<this exe> --prepare-shaders` as its own below-normal, windowless process. False with `err` when it could not be started.
bool start_detached(std::string& err);
// Asks a running preparation to stop after the current ride (creates <cache_dir>\prepare.stop).
void request_stop(const std::string& cache_dir);

}  // namespace snap_shaderprep
