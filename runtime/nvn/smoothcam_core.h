// SPDX-License-Identifier: MIT
// Smooth camera, the parts without a GPU (project code, ours): the in-between camera and the present schedule. Used by
// runtime/nvn/nvn_smoothcam.cpp and tested by tests/hle/hle_tests.cpp ("smoothcam.core").
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace snap_smoothcam {

struct M4 { double m[4][4]; };

inline M4 Mul(const M4& a, const M4& b) {
  M4 r{};
  for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double s = 0; for (int k = 0; k < 4; k++) s += a.m[i][k] * b.m[k][j]; r.m[i][j] = s; }
  return r;
}
inline bool Invert(const M4& a, M4& r) {   // Gauss-Jordan with partial pivoting
  double t[4][8];
  for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { t[i][j] = a.m[i][j]; t[i][j + 4] = i == j ? 1.0 : 0.0; }
  for (int c = 0; c < 4; c++) {
    int p = c;
    for (int i = c + 1; i < 4; i++) if (std::fabs(t[i][c]) > std::fabs(t[p][c])) p = i;
    if (std::fabs(t[p][c]) < 1e-14) return false;
    if (p != c) for (int j = 0; j < 8; j++) std::swap(t[p][j], t[c][j]);
    const double inv = 1.0 / t[c][c];
    for (int j = 0; j < 8; j++) t[c][j] *= inv;
    for (int i = 0; i < 4; i++) if (i != c) { const double f = t[i][c]; if (f != 0) for (int j = 0; j < 8; j++) t[i][j] -= f * t[c][j]; }
  }
  for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r.m[i][j] = t[i][j + 4];
  return true;
}
inline M4 Lerp(const M4& a, const M4& b, double t) {
  M4 r{};
  for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r.m[i][j] = a.m[i][j] + (b.m[i][j] - a.m[i][j]) * t;
  return r;
}

// rotation matrix <-> unit quaternion (w, x, y, z)
inline void ToQuat(const double R[3][3], double q[4]) {
  const double tr = R[0][0] + R[1][1] + R[2][2];
  if (tr > 0) { const double s = std::sqrt(tr + 1.0) * 2; q[0] = 0.25 * s; q[1] = (R[2][1] - R[1][2]) / s; q[2] = (R[0][2] - R[2][0]) / s; q[3] = (R[1][0] - R[0][1]) / s; }
  else if (R[0][0] > R[1][1] && R[0][0] > R[2][2]) { const double s = std::sqrt(1.0 + R[0][0] - R[1][1] - R[2][2]) * 2; q[0] = (R[2][1] - R[1][2]) / s; q[1] = 0.25 * s; q[2] = (R[0][1] + R[1][0]) / s; q[3] = (R[0][2] + R[2][0]) / s; }
  else if (R[1][1] > R[2][2]) { const double s = std::sqrt(1.0 + R[1][1] - R[0][0] - R[2][2]) * 2; q[0] = (R[0][2] - R[2][0]) / s; q[1] = (R[0][1] + R[1][0]) / s; q[2] = 0.25 * s; q[3] = (R[1][2] + R[2][1]) / s; }
  else { const double s = std::sqrt(1.0 + R[2][2] - R[0][0] - R[1][1]) * 2; q[0] = (R[1][0] - R[0][1]) / s; q[1] = (R[0][2] + R[2][0]) / s; q[2] = (R[1][2] + R[2][1]) / s; q[3] = 0.25 * s; }
}
inline void FromQuat(const double q[4], double R[3][3]) {
  const double w = q[0], x = q[1], y = q[2], z = q[3];
  R[0][0] = 1 - 2 * (y * y + z * z); R[0][1] = 2 * (x * y - z * w); R[0][2] = 2 * (x * z + y * w);
  R[1][0] = 2 * (x * y + z * w); R[1][1] = 1 - 2 * (x * x + z * z); R[1][2] = 2 * (y * z - x * w);
  R[2][0] = 2 * (x * z - y * w); R[2][1] = 2 * (y * z + x * w); R[2][2] = 1 - 2 * (x * x + y * y);
}
inline void Slerp(const double a[4], const double b0[4], double t, double o[4]) {
  double b[4] = {b0[0], b0[1], b0[2], b0[3]};
  double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  if (d < 0) { d = -d; for (double& v : b) v = -v; }
  double ka = 1 - t, kb = t;
  if (d < 0.9995) { const double th = std::acos(d), s = std::sin(th); ka = std::sin((1 - t) * th) / s; kb = std::sin(t * th) / s; }
  double n = 0;
  for (int i = 0; i < 4; i++) { o[i] = ka * a[i] + kb * b[i]; n += o[i] * o[i]; }
  n = std::sqrt(n);
  for (int i = 0; i < 4; i++) o[i] /= n;
}

// A game camera: viewProjection and projection (row-major, clip = M * p) and its position.
struct Camera { M4 vp{}, proj{}; double eye[3] = {}; };

// The in-between camera at t (0 = a, 1 = b): rotation slerped, eye and projection interpolated linearly. view = inverse(P) * VP is a rigid
// transform for the game's cameras; when it is not, the viewProjection itself is interpolated. eyeOut gets the interpolated position.
inline M4 Between(const Camera& a, const Camera& b, double t, double eyeOut[3]) {
  for (int i = 0; i < 3; i++) eyeOut[i] = a.eye[i] + (b.eye[i] - a.eye[i]) * t;
  M4 ia, ib;
  if (!Invert(a.proj, ia) || !Invert(b.proj, ib)) return Lerp(a.vp, b.vp, t);
  const M4 va = Mul(ia, a.vp), vb = Mul(ib, b.vp);
  auto rigid = [](const M4& v) {
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
      double s = 0; for (int k = 0; k < 3; k++) s += v.m[i][k] * v.m[j][k];
      if (std::fabs(s - (i == j ? 1.0 : 0.0)) > 2e-3) return false;
    }
    return std::fabs(v.m[3][0]) < 1e-6 && std::fabs(v.m[3][1]) < 1e-6 && std::fabs(v.m[3][2]) < 1e-6 && std::fabs(v.m[3][3] - 1) < 1e-6;
  };
  if (!rigid(va) || !rigid(vb)) return Lerp(a.vp, b.vp, t);
  double Ra[3][3], Rb[3][3], Rt[3][3], qa[4], qb[4], qt[4], et[3];
  for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { Ra[i][j] = va.m[i][j]; Rb[i][j] = vb.m[i][j]; }
  for (int i = 0; i < 3; i++) {   // eye of a view matrix = -R^T * translation
    const double ea = -(Ra[0][i] * va.m[0][3] + Ra[1][i] * va.m[1][3] + Ra[2][i] * va.m[2][3]);
    const double eb = -(Rb[0][i] * vb.m[0][3] + Rb[1][i] * vb.m[1][3] + Rb[2][i] * vb.m[2][3]);
    et[i] = ea + (eb - ea) * t;
  }
  ToQuat(Ra, qa); ToQuat(Rb, qb); Slerp(qa, qb, t, qt); FromQuat(qt, Rt);
  M4 vt{};
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) vt.m[i][j] = Rt[i][j];
    vt.m[i][3] = -(Rt[i][0] * et[0] + Rt[i][1] * et[1] + Rt[i][2] * et[2]);
  }
  vt.m[3][3] = 1;
  return Mul(Lerp(a.proj, b.proj, t), vt);
}

// yaw / pitch (degrees) of a viewProjection's forward axis (minus its w row, as upscale.cpp TakeCamera finds it)
inline void Angles(const M4& vp, double* yaw, double* pitch) {
  double f[3] = {-vp.m[3][0], -vp.m[3][1], -vp.m[3][2]};
  const double n = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  if (n <= 0) { *yaw = *pitch = 0; return; }
  for (double& v : f) v /= n;
  *yaw = std::atan2(f[0], f[2]) * 57.29577951308232;
  *pitch = std::asin(std::clamp(f[1], -1.0, 1.0)) * 57.29577951308232;
}

// Present schedule of one game frame. Game frame N was released at `now` (R_N), the one before at lastRel (R_{N-1}; 0 = none).
// Grid points t, P apart, show the camera of time t - D: those with t - D in (R_{N-1}, R_N] belong to this frame, with
// alpha = (t - D - R_{N-1}) / (R_N - R_{N-1}). gridNext carries the grid from frame to frame (0 = start a new grid). Points more than
// half a period in the past are skipped (counted in *late), but the last one is always kept, so a frame is never dropped.
// Returns the number of passes (1..kMax) written to alpha / warp / due.
inline int PlanFrame(int64_t now, int64_t lastRel, int64_t P, int64_t D, bool warpOk, int64_t* gridNext, int kMax, float* alpha, bool* warp,
                     int64_t* due, uint64_t* late) {
  const int64_t B = lastRel ? now - lastRel : 0;
  if (B <= 0) warpOk = false;
  if (*gridNext == 0 || *gridNext < now - P) *gridNext = std::max(now, lastRel ? lastRel + D + P / 2 : now + D);
  int64_t ts[64]; int nt = 0;
  for (int64_t t = *gridNext; t <= now + D && nt < 64; t += P) ts[nt++] = t;
  int first = 0;
  while (first < nt - 1 && ts[first] < now - P / 2) { first++; if (late) (*late)++; }
  if (nt - first > kMax) first = nt - kMax;
  if (nt == 0) { ts[0] = *gridNext; nt = 1; first = 0; }   // the game ran faster than the grid this frame: one present at the next point
  const int n = nt - first;
  for (int i = 0; i < n; i++) {
    const int64_t t = ts[first + i];
    double a = 1.0;
    if (warpOk) a = std::clamp((double)(t - D - lastRel) / (double)B, 0.0, 1.0);
    alpha[i] = (float)a;
    warp[i] = warpOk && a < 0.999;
    due[i] = t;
  }
  *gridNext = ts[nt - 1] + P;
  return n;
}
}  // namespace snap_smoothcam
