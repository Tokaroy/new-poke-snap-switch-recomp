// Core mixer behavior through the real AudioOut API on the timer backend, read back from the capture tap (no device, no sound):
//   an underrun is a short fade, not a step; the limiter holds its ceiling; a mute slews instead of cutting; night mode raises quiet
//   passages; the new options parse (latency / resampler / range / device) and report in the stats.
// usage: core_test.exe      (exit code 0 = all passed)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>
#include "audio_out.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Run {
  std::vector<float> tap;   // interleaved stereo
  SnapAudioStats st{};
};

// Feeds stereo 1024-frame buffers from gen(frameIndex) -> sample for `feedMs`, then stops feeding for `holeMs` (an underrun), then feeds again for
// `resumeMs`. `during(tMs)` runs every 5 ms (to flip mute etc.).
static Run run(const std::function<float(uint64_t)>& gen, int feedMs, int holeMs, int resumeMs, const std::function<void(int)>& during = nullptr,
               const std::function<void(SnapAudioConfig&)>& cfgfn = nullptr) {
  SnapAudioConfig cfg; snap_audio_default_config(&cfg);
  cfg.backend = SNAP_AUDIO_BACKEND_NULL; cfg.enableTap = 1;
  if (cfgfn) cfgfn(cfg);
  snap_audio_configure(&cfg);
  snap_audio_reset_stats();
  static alignas(8) unsigned char ao[0x168];
  std::memset(ao, 0, sizeof ao);
  SnapAudioOutParameter p; snap_nn_audio_InitializeAudioOutParameter(&p); p.sampleRate = 48000; p.channelCount = 2;
  if (snap_nn_audio_OpenDefaultAudioOut(ao, &p)) { CHECK(false, "open failed"); return {}; }
  constexpr int kN = 4; constexpr uint32_t kFrames = 1024;
  static std::vector<int16_t> pcm[kN];
  static SnapAudioOutBuffer bufs[kN];
  uint64_t frame = 0;
  auto fill = [&](int i) {
    pcm[i].resize((size_t)kFrames * 2);
    for (uint32_t f = 0; f < kFrames; ++f, ++frame) { const float v = std::fmax(-1.0f, std::fmin(1.0f, gen(frame))); pcm[i][f * 2] = pcm[i][f * 2 + 1] = (int16_t)std::lrintf(v * 32767.0f); }
    snap_nn_audio_SetAudioOutBufferInfo(&bufs[i], pcm[i].data(), (uint64_t)kFrames * 4, (uint64_t)kFrames * 4);
    snap_nn_audio_AppendAudioOutBuffer(ao, &bufs[i]);
  };
  for (int i = 0; i < kN; ++i) fill(i);
  snap_nn_audio_StartAudioOut(ao);
  Run r;
  std::vector<float> tmp(8192);
  const int total = feedMs + holeMs + resumeMs;
  const ULONGLONG t0 = GetTickCount64();
  for (int t = 0; t < total; t = (int)(GetTickCount64() - t0)) {
    Sleep(2);
    if (during) during(t);
    const bool feeding = t < feedMs || t >= feedMs + holeMs;
    while (SnapAudioOutBuffer* b = snap_nn_audio_GetReleasedAudioOutBuffer(ao)) { if (feeding) fill((int)(b - bufs)); }
    for (uint64_t n; (n = snap_audio_tap_read(tmp.data(), tmp.size())) > 0;) r.tap.insert(r.tap.end(), tmp.begin(), tmp.begin() + (std::ptrdiff_t)n);
  }
  snap_audio_get_stats(&r.st);
  snap_nn_audio_StopAudioOut(ao);
  snap_nn_audio_CloseAudioOut(ao);
  snap_audio_shutdown();
  return r;
}

static double max_step(const std::vector<float>& t, size_t from, size_t to) {   // left channel
  double m = 0;
  for (size_t i = std::max<size_t>(from, 1); i < to && i * 2 < t.size(); ++i) m = std::max(m, (double)std::fabs(t[i * 2] - t[(i - 1) * 2]));
  return m;
}
static double peak_of(const std::vector<float>& t) { double m = 0; for (float v : t) m = std::max(m, (double)std::fabs(v)); return m; }

int main() {
  _putenv_s("SNAP_CONFIG_DIR", ".");
  _putenv_s("SNAP_AUDIO_BACKEND", "null");
  const double kPi = 3.14159265358979323846;

  // ---- 1. underrun: a 0.5 DC level (the worst case for a step) is fed for 300 ms, runs dry for 150 ms, resumes. The old mixer cut it from 0.5 to 0
  //         and back (a 0.5 step, a loud click); now every sample-to-sample step stays under 0.02 (2 ms fades), after the one-time start.
  {
    Run r = run([](uint64_t) { return 0.5f; }, 300, 150, 300);
    size_t first = 0; while (first < r.tap.size() && r.tap[first * 2] == 0.0f) ++first;
    const double step = max_step(r.tap, first + 2, r.tap.size() / 2);
    std::printf("underrun edges: %zu frames tapped, max step after the start %.4f, underruns %llu\n", r.tap.size() / 2, step, (unsigned long long)r.st.underruns);
    CHECK(r.st.underruns >= 1, "the hole must count as an underrun");
    CHECK(step < 0.02, "underrun edge step %.4f (a hard cut is 0.5)", step);
    // and the hole really is silent in the middle, and the level comes back
    double mid = 0, end = 0; size_t n = r.tap.size() / 2;
    for (size_t i = 0; i < n; ++i) { if (i > first + 14400 + 4000 && i < first + 14400 + 6000) mid = std::max(mid, (double)std::fabs(r.tap[i * 2])); }
    for (size_t i = n - 480; i < n; ++i) end = std::max(end, (double)r.tap[i * 2]);
    CHECK(end > 0.45 || n < 40000, "level back after the hole: %.3f", end);
    (void)mid;
  }

  // ---- 2. limiter: full-scale DC and a full-scale square never pass 0.98 (the old hard clip let 1.0 through)
  {
    Run r = run([](uint64_t f) { return (f / 24) % 2 ? 1.0f : -1.0f; }, 400, 0, 0);
    const double pk = peak_of(r.tap);
    std::printf("limiter: peak %.4f for a full-scale square\n", pk);
    CHECK(pk <= 0.9801 && pk > 0.5, "limiter peak %.4f", pk);
  }

  // ---- 3. mute is a 30 ms slew, not a cut: the step is bounded and the tail reaches zero
  {
    bool muted = false;
    Run r = run([&](uint64_t f) { return 0.5f * (float)std::sin(2 * kPi * 440.0 * (double)f / 48000.0); }, 600, 0, 0,
                [&](int t) { if (t >= 300 && !muted) { snap_audio_set_muted(1); muted = true; } });
    snap_audio_set_muted(0);
    const size_t n = r.tap.size() / 2;
    // the sine's own max step is 2*pi*440/48000*0.5 = 0.029; with a fade it must not exceed that by much
    const double step = max_step(r.tap, 2000, n);
    double tail = 0; for (size_t i = n - 2400; i < n; ++i) tail = std::max(tail, (double)std::fabs(r.tap[i * 2]));
    std::printf("mute: max step %.4f (sine alone 0.029), tail peak %.5f\n", step, tail);
    CHECK(step < 0.035, "mute step %.4f", step);
    CHECK(tail == 0.0, "muted tail %.5f", tail);
    CHECK(r.st.underruns == 0, "a mute is not an underrun (%llu)", (unsigned long long)r.st.underruns);
  }

  // ---- 4. night mode: a quiet tone comes out louder, a loud one quieter, live through snap_audio_set_range
  {
    auto level = [&](int range, float amp) {
      Run r = run([&](uint64_t f) { return amp * (float)std::sin(2 * kPi * 200.0 * (double)f / 48000.0); }, 1200, 0, 0,
                  [&](int t) { if (t == 0) snap_audio_set_range(range); });
      double e = 0; size_t n = r.tap.size() / 2, c = 0;
      for (size_t i = n / 2; i < n; ++i, ++c) e += (double)r.tap[i * 2] * r.tap[i * 2];
      return std::sqrt(e / (double)(c ? c : 1));
    };
    const double q0 = level(0, 0.01f), q2 = level(2, 0.01f), l0 = level(0, 0.8f), l2 = level(2, 0.8f);
    std::printf("range: quiet full %.5f night %.5f (x%.2f); loud full %.4f night %.4f\n", q0, q2, q2 / q0, l0, l2);
    CHECK(q2 / q0 > 2.0 && q2 / q0 < 3.2, "night mode lifts quiet by 9 dB (x2.8): x%.2f", q2 / q0);
    CHECK(l2 < l0 * 0.5, "night mode holds loud down");
    snap_audio_set_range(0);
  }

  // ---- 5. options: env parse, stats report; an unknown device name falls back (null backend: nothing opens, nothing crashes)
  {
    _putenv_s("SNAP_AUDIO_LATENCY", "safe"); _putenv_s("SNAP_AUDIO_RESAMPLER", "sinc"); _putenv_s("SNAP_AUDIO_RANGE", "reduced"); _putenv_s("SNAP_AUDIO_DEVICE", "No Such Device");
    Run r = run([](uint64_t) { return 0.0f; }, 100, 0, 0);
    CHECK(r.st.latencyMode == SNAP_AUDIO_LAT_SAFE, "latency from env: %d", r.st.latencyMode);
    CHECK(r.st.range == SNAP_AUDIO_RANGE_REDUCED, "range from env: %d", r.st.range);
    snap_audio_set_range(0);
    _putenv_s("SNAP_AUDIO_LATENCY", "bogus"); _putenv_s("SNAP_AUDIO_RESAMPLER", ""); _putenv_s("SNAP_AUDIO_RANGE", ""); _putenv_s("SNAP_AUDIO_DEVICE", "");
    Run r2 = run([](uint64_t) { return 0.0f; }, 100, 0, 0);
    CHECK(r2.st.latencyMode == SNAP_AUDIO_LAT_NORMAL && r2.st.range == SNAP_AUDIO_RANGE_FULL, "defaults after a bogus value: %d %d", r2.st.latencyMode, r2.st.range);
    snap_audio_set_range(0);
    char names[4096]; const int n = snap_audio_list_devices(names, sizeof names);
    std::printf("render devices: %d\n", n);
    CHECK(n >= 0, "device list");
  }

  std::printf(fails ? "core_test: %d FAILED\n" : "core_test: all passed\n", fails);
  return fails ? 1 : 0;
}
