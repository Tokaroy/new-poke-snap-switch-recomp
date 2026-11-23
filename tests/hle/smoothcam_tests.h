// SPDX-License-Identifier: MIT
// Tests for the smooth camera's GPU-free half (runtime/nvn/smoothcam_core.h): the in-between camera and the present schedule.
// Included by hle_tests.cpp (uses its CHECK macros).
#pragma once
#include <cmath>
#include <vector>
#include "../../runtime/nvn/smoothcam_core.h"

namespace smoothcam_test {
namespace sc = snap_smoothcam;

// A game-like camera: yaw about +y, eye on the ground plane, reversed infinite projection (P22 = 0, as the course camera has).
inline sc::Camera make_cam(double yawDeg, double ex, double ez, double fovY = 0.6) {
  const double th = yawDeg * 3.14159265358979323846 / 180.0, c = std::cos(th), s = std::sin(th);
  const double R[3][3] = {{c, 0, -s}, {0, 1, 0}, {s, 0, c}};
  const double e[3] = {ex, 1.5, ez};
  sc::M4 V{};
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) V.m[i][j] = R[i][j];
    V.m[i][3] = -(R[i][0] * e[0] + R[i][1] * e[1] + R[i][2] * e[2]);
  }
  V.m[3][3] = 1;
  const double f = 1.0 / std::tan(fovY / 2), n = 0.1;
  sc::M4 P{};
  P.m[0][0] = f / (16.0 / 9.0); P.m[1][1] = f; P.m[2][3] = n; P.m[3][2] = -1;
  sc::Camera k; k.proj = P; k.vp = sc::Mul(P, V);
  for (int i = 0; i < 3; i++) k.eye[i] = e[i];
  return k;
}
inline double max_diff(const sc::M4& a, const sc::M4& b) {
  double d = 0;
  for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) d = std::max(d, std::fabs(a.m[i][j] - b.m[i][j]));
  return d;
}

inline void test_core() {
  const sc::Camera a = make_cam(10.0, 0.0, 0.0), b = make_cam(13.0, 0.05, 0.4, 0.55);
  double eye[3];
  CHECK(max_diff(sc::Between(a, b, 0.0, eye), a.vp) < 1e-9);   // the ends are the game's own cameras
  CHECK(max_diff(sc::Between(a, b, 1.0, eye), b.vp) < 1e-9);
  CHECK(std::fabs(eye[0] - 0.05) < 1e-12 && std::fabs(eye[2] - 0.4) < 1e-12);
  double ya, pa, yb, pb, ym, pm;
  sc::Angles(a.vp, &ya, &pa); sc::Angles(b.vp, &yb, &pb); sc::Angles(sc::Between(a, b, 0.5, eye), &ym, &pm);
  CHECK(std::fabs(std::fabs(yb - ya) - 3.0) < 1e-6);            // a 3 degree turn
  CHECK(std::fabs(ym - (ya + yb) / 2) < 1e-6);                  // the in-between camera turned half of it
  CHECK(std::fabs(eye[2] - 0.2) < 1e-12);
  // the re-projection matrix of the newest frame to itself is the identity
  sc::M4 inv;
  CHECK(sc::Invert(b.vp, inv));
  sc::M4 id{}; for (int i = 0; i < 4; i++) id.m[i][i] = 1;
  CHECK(max_diff(sc::Mul(sc::Between(a, b, 1.0, eye), inv), id) < 1e-9);
  // a non-rigid view (scaled) falls back to interpolating the viewProjection itself
  sc::Camera s = b; for (int j = 0; j < 4; j++) s.vp.m[0][j] *= 1.5;
  CHECK(max_diff(sc::Between(a, s, 0.25, eye), sc::Lerp(a.vp, s.vp, 0.25)) < 1e-12);
}

// 60 fps game on a 144 Hz display: one present per display period, every one a new camera, equal steps for a steady turn.
inline void schedule_run(double jitterMs, std::vector<int64_t>* dues, std::vector<double>* shownYaw, std::vector<int>* perFrame, uint64_t* late) {
  const int64_t P = (int64_t)(1e9 / 144.0), B = 16666667, D = B + 1000000;
  const double w = 1.5;   // degrees per game frame (90 degrees a second)
  int64_t grid = 0, lastRel = 0;
  for (int k = 0; k < 240; k++) {
    const double j = jitterMs * ((k * 7919) % 5 - 2) / 2.0;   // -jitter..+jitter, deterministic
    const int64_t rel = 1000000000LL + k * B + (int64_t)(j * 1e6);
    float alpha[8]; bool warp[8]; int64_t due[8];
    const int n = sc::PlanFrame(rel, lastRel, P, D, k > 0, &grid, 8, alpha, warp, due, late);
    perFrame->push_back(n);
    for (int i = 0; i < n; i++) { dues->push_back(due[i]); shownYaw->push_back(((k - 1) + alpha[i]) * w); CHECK(alpha[i] >= 0.0f && alpha[i] <= 1.0f); }
    lastRel = rel;
  }
}

inline void test_schedule() {
  const int64_t P = (int64_t)(1e9 / 144.0);
  {   // steady 60 fps
    std::vector<int64_t> dues; std::vector<double> yaw; std::vector<int> per; uint64_t late = 0;
    schedule_run(0.0, &dues, &yaw, &per, &late);
    CHECK_EQ(late, 0u);
    bool even = true;
    for (size_t i = 1; i < dues.size(); i++) if (dues[i] - dues[i - 1] != P) even = false;
    CHECK(even);                                                  // one present per display period, none skipped or doubled
    int two = 0, three = 0, other = 0;
    for (size_t k = 2; k < per.size(); k++) (per[k] == 2 ? two : per[k] == 3 ? three : other)++;
    CHECK(other == 0 && two > 0 && three > 0);                    // 2.4 passes a game frame
    CHECK(std::fabs((double)dues.size() / per.size() - 2.4) < 0.02);
    // camera steps between consecutive presents (skip the first two game frames, before the grid settled)
    double mn = 1e9, mx = -1e9;
    for (size_t i = 8; i < yaw.size(); i++) { const double d = yaw[i] - yaw[i - 1]; mn = std::min(mn, d); mx = std::max(mx, d); }
    const double ideal = 1.5 * 60.0 / 144.0;                     // 0.625 degrees per display period
    CHECK(mn > ideal * 0.98 && mx < ideal * 1.02);                // steady, no 60 Hz stair steps
  }
  {   // release jitter of +-2 ms: the grid keeps its period, every frame is shown, the steps stay within 25%
    std::vector<int64_t> dues; std::vector<double> yaw; std::vector<int> per; uint64_t late = 0;
    schedule_run(2.0, &dues, &yaw, &per, &late);
    bool even = true;
    for (size_t i = 1; i < dues.size(); i++) if (dues[i] - dues[i - 1] != P) even = false;
    CHECK(even);
    bool shown = true; for (int n : per) if (n < 1) shown = false;
    CHECK(shown);
    double mn = 1e9, mx = -1e9;
    for (size_t i = 8; i < yaw.size(); i++) { const double d = yaw[i] - yaw[i - 1]; mn = std::min(mn, d); mx = std::max(mx, d); }
    CHECK(mn > 0.625 * 0.75 && mx < 0.625 * 1.25);
  }
  {   // a frame that arrives 40 ms late: the grid restarts at its release (no burst of stale points), alphas stay in range
    int64_t grid = 0; float alpha[8]; bool warp[8]; int64_t due[8]; uint64_t late = 0;
    sc::PlanFrame(1000000000LL, 983333333LL, P, 17666667, true, &grid, 8, alpha, warp, due, &late);
    const int n = sc::PlanFrame(1056666667LL, 1000000000LL, P, 17666667, true, &grid, 8, alpha, warp, due, &late);
    CHECK(n >= 1 && due[0] >= 1056666667LL - P / 2 && alpha[n - 1] <= 1.0f && alpha[0] >= 0.0f);
  }
}
}  // namespace smoothcam_test
