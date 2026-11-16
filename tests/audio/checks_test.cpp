// SPDX-License-Identifier: MIT
// Host-only test of the AudioOut parameter checks (docs/audio.md, "Checks"): feeds out-of-range open parameters, dead handles,
// broken buffers, bad volumes and bad device pulls into the nn::audio layer and expects a clean rejection (no crash, a counter, one
// log line per kind), and that a good buffer still plays afterwards.
//
// usage: checks_test.exe          (null backend, no sound)
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "audio_internal.h"
#include "audio_out.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct Out { alignas(16) unsigned char mem[0x168]; };

static SnapAudioChecks checks() { SnapAudioChecks c; snap_audio_get_checks(&c); return c; }

static bool open_out(Out& o, uint32_t rate = 48000, uint16_t ch = 2) {
  std::memset(o.mem, 0, sizeof o.mem);
  SnapAudioOutParameter p;
  snap_nn_audio_InitializeAudioOutParameter(&p);
  p.sampleRate = rate;
  p.channelCount = ch;
  return snap_nn_audio_OpenDefaultAudioOut(o.mem, &p) == 0;
}

// polls GetReleased until n buffers came back (the device thread does the releasing)
static int drain(Out& o, int n, int ms) {
  int got = 0;
  for (int t = 0; t < ms && got < n; ++t) {
    while (snap_nn_audio_GetReleasedAudioOutBuffer(o.mem)) ++got;
    if (got < n) Sleep(1);
  }
  return got;
}

static int count_lines(const std::string& text, const char* needle) {
  int n = 0;
  for (size_t p = text.find(needle); p != std::string::npos; p = text.find(needle, p + 1)) ++n;
  return n;
}

int main() {
  _putenv_s("SNAP_AUDIO_BACKEND", "null");
  char logPath[MAX_PATH], cfgDir[MAX_PATH];
  GetTempPathA(MAX_PATH, logPath);
  std::strcpy(cfgDir, logPath);
  std::strcat(cfgDir, "snap_audio_checks_cfg");
  CreateDirectoryA(cfgDir, nullptr);
  _putenv_s("SNAP_CONFIG_DIR", cfgDir);   // set_volume writes audio.json: keep it away from the real one
  std::strcat(logPath, "snap_audio_checks_stderr.txt");
  std::freopen(logPath, "w", stderr);   // the log lines are part of what is checked

  snap_audio_reset_stats();
  CHECK(checks().badOpen == 0 && checks().badHandle == 0);

  // ---- sink parameters ----------------------------------------------------------------------------------------------
  Out o[5];
  SnapAudioOutParameter good;
  snap_nn_audio_InitializeAudioOutParameter(&good);
  CHECK(snap_nn_audio_OpenDefaultAudioOut(nullptr, &good) != 0);                 // null AudioOut
  CHECK(snap_nn_audio_OpenDefaultAudioOut(o[0].mem, nullptr) != 0);              // null parameters
  CHECK(snap_nn_audio_OpenDefaultAudioOut(o[0].mem + 1, &good) != 0);            // misaligned
  CHECK(!open_out(o[0], 44100, 2));                                              // rate the device does not run
  CHECK(!open_out(o[0], 48000, 3));                                              // channel counts nobody mixes
  CHECK(!open_out(o[0], 48000, 8));
  CHECK(!open_out(o[0], 0xffffffffu, 2));
  CHECK(checks().badOpen == 7);
  CHECK(*reinterpret_cast<uint32_t*>(o[0].mem) == 0);                            // a refused open leaves the handle closed

  for (int i = 0; i < 4; ++i) CHECK(open_out(o[i]));
  CHECK(!open_out(o[4]));                                                        // all four AudioOuts are open
  CHECK(!open_out(o[4]));
  CHECK(checks().noSlot == 2);
  for (int i = 0; i < 4; ++i) snap_nn_audio_CloseAudioOut(o[i].mem);

  // ---- dead handles -------------------------------------------------------------------------------------------------
  Out dead;   // never opened
  std::memset(dead.mem, 0, sizeof dead.mem);
  Out junk;   // looks like something else: the magic is wrong, the slot index is wild
  std::memset(junk.mem, 0xAB, sizeof junk.mem);
  SnapAudioOutBuffer b;
  alignas(16) int16_t pcm[256] = {};
  snap_nn_audio_SetAudioOutBufferInfo(&b, pcm, sizeof pcm, sizeof pcm);
  const uint64_t before = checks().badHandle;
  for (void* h : {(void*)nullptr, (void*)dead.mem, (void*)junk.mem}) {
    CHECK(snap_nn_audio_StartAudioOut(h) != 0);
    snap_nn_audio_StopAudioOut(h);
    snap_nn_audio_AppendAudioOutBuffer(h, &b);
    CHECK(snap_nn_audio_GetReleasedAudioOutBuffer(h) == nullptr);
    CHECK(snap_nn_audio_GetAudioOutChannelCount(h) == 0);
    CHECK(snap_nn_audio_GetAudioOutState(h) == SNAP_AUDIO_STATE_STOPPED);        // a state query is not counted
    snap_nn_audio_CloseAudioOut(h);
  }
  CHECK(checks().badHandle - before == 18);                                      // 6 counted calls on each of 3 handles
  CHECK(snap_nn_audio_GetAudioOutBufferDataPointer(nullptr) == nullptr);
  snap_nn_audio_SetAudioOutBufferInfo(nullptr, pcm, 1, 1);                       // counted as a bad buffer, no crash

  // ---- buffers ------------------------------------------------------------------------------------------------------
  CHECK(open_out(o[0]));
  CHECK(snap_nn_audio_StartAudioOut(o[0].mem) == 0);
  const uint64_t bad0 = checks().badBuffer;
  alignas(16) static int16_t big[4096];
  std::memset(big, 0x11, sizeof big);
  SnapAudioOutBuffer bufs[6];
  snap_nn_audio_SetAudioOutBufferInfo(&bufs[0], nullptr, 4096, 4096);            // data pointer null
  snap_nn_audio_SetAudioOutBufferInfo(&bufs[1], big, sizeof big, 1ull << 40);    // absurd size
  snap_nn_audio_SetAudioOutBufferInfo(&bufs[2], big, 1024, 4096);                // data runs past the buffer
  snap_nn_audio_SetAudioOutBufferInfo(&bufs[3], big, sizeof big, 1002);          // not whole stereo frames (4 bytes)
  snap_nn_audio_SetAudioOutBufferInfo(&bufs[4], big, 1024, 512);
  bufs[4].dataOffset = 1ull << 33;                                               // offset far outside the buffer
  snap_nn_audio_SetAudioOutBufferInfo(&bufs[5], big, 2048, 1024);                // fine: 256 frames
  snap_nn_audio_AppendAudioOutBuffer(o[0].mem, nullptr);                         // null buffer: counted, nothing to release
  for (auto& x : bufs) snap_nn_audio_AppendAudioOutBuffer(o[0].mem, &x);
  CHECK(checks().badBuffer - bad0 == 6);                                         // null + five broken (the good one is not counted)
  CHECK(drain(o[0], 6, 3000) == 6);                                              // all six come back, the broken ones trimmed
  CHECK(bufs[0].dataSize == 0);
  CHECK(bufs[1].dataSize == 0);
  CHECK(bufs[2].dataSize == 1024 && bufs[2].dataOffset == 0);
  CHECK(bufs[3].dataSize == 1000);
  CHECK(bufs[4].dataSize <= 1024 && bufs[4].dataOffset <= 1024);
  CHECK(bufs[5].dataSize == 1024);
  snap_nn_audio_StopAudioOut(o[0].mem);
  snap_nn_audio_CloseAudioOut(o[0].mem);

  // ---- queue full: 64 submits, no consumer (the AudioOut is not started), then one more ---------------------------------
  CHECK(open_out(o[0]));
  static SnapAudioOutBuffer many[66];
  for (int i = 0; i < 66; ++i) snap_nn_audio_SetAudioOutBufferInfo(&many[i], big, sizeof big, 256);
  const uint64_t full0 = checks().queueFull;
  for (int i = 0; i < 66; ++i) snap_nn_audio_AppendAudioOutBuffer(o[0].mem, &many[i]);
  CHECK(checks().queueFull - full0 >= 1 && checks().queueFull - full0 <= 3);
  snap_nn_audio_CloseAudioOut(o[0].mem);

  // ---- volumes ------------------------------------------------------------------------------------------------------
  const uint64_t vol0 = checks().badVolume;
  snap_audio_set_volume(0, 0.5f);
  snap_audio_set_volume(0, NAN);
  snap_audio_set_volume(0, INFINITY);
  snap_audio_set_volume(-1, 0.5f);
  snap_audio_set_volume(99, 0.5f);
  CHECK(std::fabs(snap_audio_get_volume(0) - 0.5f) < 1e-6f);                     // the good value survived the bad ones
  SnapAudioDeviceName nm;
  std::memset(nm.name, 0, sizeof nm.name);
  std::strcpy(nm.name, "AudioTvOutput");
  snap_nn_audio_SetAudioDeviceOutputVolume(&nm, NAN);
  snap_nn_audio_SetAudioDeviceOutputVolume(nullptr, 0.5f);
  std::strcpy(nm.name, "NoSuchDevice");
  snap_nn_audio_SetAudioDeviceOutputVolume(&nm, 0.5f);
  CHECK(snap_nn_audio_ListAudioDeviceName(nullptr, 4) == 0);
  CHECK(checks().badVolume - vol0 == 7);
  snap_audio_set_volume(0, 1.0f);

  // ---- device pulls -------------------------------------------------------------------------------------------------
  const uint64_t pull0 = checks().badPull;
  float buf[64];
  std::memset(buf, 0x7f, sizeof buf);
  snap_audio::core_mix(nullptr, 480);
  snap_audio::core_mix(buf, 1u << 20);
  CHECK(checks().badPull - pull0 == 2);
  CHECK(buf[0] != 0.0f);                                                         // a rejected pull wrote nothing
  snap_audio::core_mix(buf, 0);                                                  // an empty pull is just ignored

  // ---- a good AudioOut after all that: plays, releases, counts nothing -----------------------------------------------------
  const uint64_t all0 = checks().badOpen + checks().noSlot + checks().badHandle + checks().badBuffer + checks().badVolume + checks().badPull;
  CHECK(open_out(o[0]));
  SnapAudioOutBuffer gb[4];
  alignas(16) static int16_t tone[1024 * 2];
  for (int i = 0; i < 1024 * 2; ++i) tone[i] = (int16_t)(8000 * std::sin(i * 0.05));
  for (auto& x : gb) { snap_nn_audio_SetAudioOutBufferInfo(&x, tone, 8192, 1024 * 2 * 2); snap_nn_audio_AppendAudioOutBuffer(o[0].mem, &x); }
  CHECK(snap_nn_audio_StartAudioOut(o[0].mem) == 0);
  CHECK(drain(o[0], 4, 3000) == 4);
  CHECK(checks().badOpen + checks().noSlot + checks().badHandle + checks().badBuffer + checks().badVolume + checks().badPull == all0);

  // ---- underruns: started, nothing queued -----------------------------------------------------------------------------------
  Sleep(300);                                                                    // the device pulls with no data left
  CHECK(checks().underruns > 0);
  SnapAudioStats st;
  snap_audio_get_stats(&st);
  CHECK(checks().underruns >= st.underruns);   // the same counter, read a moment apart
  snap_nn_audio_StopAudioOut(o[0].mem);
  snap_nn_audio_CloseAudioOut(o[0].mem);

  snap_audio_log_checks();
  std::fflush(stderr);
  snap_audio_shutdown();
  std::fflush(stderr);
  std::freopen("CON", "w", stderr);

  // ---- one log line per kind --------------------------------------------------------------------------------------------------
  std::string text;
  if (FILE* f = std::fopen(logPath, "rb")) {
    char tmp[4096]; size_t n;
    while ((n = std::fread(tmp, 1, sizeof tmp, f)) > 0) text.append(tmp, n);
    std::fclose(f);
  }
  CHECK(count_lines(text, "[audio] check: bad open parameters") == 1);
  CHECK(count_lines(text, "[audio] check: no free AudioOut") == 1);
  CHECK(count_lines(text, "[audio] check: AudioOut not open") == 1);
  CHECK(count_lines(text, "[audio] check: bad buffer") == 1);
  CHECK(count_lines(text, "[audio] check: submit queue full") == 1);
  CHECK(count_lines(text, "[audio] check: bad volume") == 1);
  CHECK(count_lines(text, "[audio] check: bad device pull") == 1);
  CHECK(count_lines(text, "[audio] underrun:") == 1);
  CHECK(count_lines(text, "[audio] checks:") == 1);
  std::remove(logPath);
  std::string cj = std::string(cfgDir) + "\\audio.json";
  DeleteFileA(cj.c_str());
  DeleteFileA((cj + ".tmp").c_str());
  RemoveDirectoryA(cfgDir);

  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
