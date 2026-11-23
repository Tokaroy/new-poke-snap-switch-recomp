// SPDX-License-Identifier: MIT
// HDR photo copies (SNAP_HDR_PHOTOS): the pure part, free of Windows and Vulkan includes so tests/hle checks it on its own.
// RECONSTRUCTS highlights from an 8-bit sRGB picture with the same inverse tone map and roll-off as the HDR present pass
// (runtime/nvn/shaders/hdr.frag): it does not capture the scene. Output: 16-bit RGB, PQ (SMPTE ST 2084), BT.2020 primaries.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace snap_hdrphoto {

struct Params { float paper = 200.f, peak = 1000.f, expand = 0.3f; };   // nits, nits, 0..1 (SNAP_HDR_PAPER_NITS, SNAP_HDR_PEAK_NITS, SNAP_HDR_EXPAND)
struct Stats { float maxCll = 0.f, maxFall = 0.f; };                     // nits: brightest max(R,G,B) of any pixel, mean of max(R,G,B) over the picture

inline float srgb_eotf(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
inline float pq_oetf(float nits) {
  const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
  const float y = std::pow(std::clamp(nits / 10000.f, 0.f, 1.f), m1);
  return std::pow((c1 + c2 * y) / (1.f + c3 * y), m2);
}
inline float rolloff(float n, float peak) {
  const float knee = 0.75f * peak;
  if (n <= knee) return n;
  return knee + (peak - knee) * (1.f - std::exp(-(n - knee) / (peak - knee)));
}

// px: rows of w*4 bytes (RGBA, or BGRA when bgra), display-encoded sRGB. out: w*h*3 uint16 (R, G, B), PQ BT.2020 full range.
inline void convert(const uint8_t* px, uint32_t w, uint32_t h, bool bgra, const Params& pr, std::vector<uint16_t>& out, Stats& st) {
  static float lut[256]; static bool have = false;
  if (!have) { for (int i = 0; i < 256; i++) lut[i] = srgb_eotf((float)i / 255.f); have = true; }
  out.resize((size_t)w * h * 3);
  const float ratio = std::max(pr.peak / pr.paper, 1.f);
  double sumMax = 0; float maxCll = 0;
  const size_t n = (size_t)w * h;
  for (size_t i = 0; i < n; i++) {
    const uint8_t* s = px + i * 4;
    float r = lut[bgra ? s[2] : s[0]], g = lut[s[1]], b = lut[bgra ? s[0] : s[2]];
    const float m = std::max(r, std::max(g, b));
    const float wt = std::clamp((m - 0.75f) / 0.25f, 0.f, 1.f);
    const float gain = 1.f + (ratio - 1.f) * 0.25f * pr.expand * wt * wt;
    r *= gain * pr.paper; g *= gain * pr.paper; b *= gain * pr.paper;
    const float mn = std::max(r, std::max(g, b));
    if (mn > 0.f) { const float k = rolloff(mn, pr.peak) / mn; r *= k; g *= k; b *= k; }
    const float R = 0.6274039f * r + 0.3292830f * g + 0.0433131f * b;
    const float G = 0.0690973f * r + 0.9195404f * g + 0.0113623f * b;
    const float B = 0.0163914f * r + 0.0880133f * g + 0.8955953f * b;
    const float mx = std::max(R, std::max(G, B));
    maxCll = std::max(maxCll, mx); sumMax += mx;
    uint16_t* d = out.data() + i * 3;
    d[0] = (uint16_t)std::lround(std::clamp(pq_oetf(std::max(R, 0.f)), 0.f, 1.f) * 65535.f);
    d[1] = (uint16_t)std::lround(std::clamp(pq_oetf(std::max(G, 0.f)), 0.f, 1.f) * 65535.f);
    d[2] = (uint16_t)std::lround(std::clamp(pq_oetf(std::max(B, 0.f)), 0.f, 1.f) * 65535.f);
  }
  st.maxCll = maxCll; st.maxFall = n ? (float)(sumMax / (double)n) : 0.f;
}

inline uint32_t crc32(const uint8_t* d, size_t n, uint32_t crc = 0) {
  static uint32_t t[256]; static bool have = false;
  if (!have) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; } have = true; }
  crc = ~crc;
  for (size_t i = 0; i < n; i++) crc = t[(crc ^ d[i]) & 255] ^ (crc >> 8);
  return ~crc;
}
inline void put32(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
inline void put16(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
inline void chunk(std::vector<uint8_t>& v, const char* type, const std::vector<uint8_t>& data) {
  put32(v, (uint32_t)data.size());
  const size_t at = v.size();
  v.insert(v.end(), type, type + 4);
  v.insert(v.end(), data.begin(), data.end());
  put32(v, crc32(v.data() + at, 4 + data.size()));
}

// Inserts cICP (BT.2020, PQ, RGB, full range), mDCv (BT.2020 primaries, D65, mastering max = peak nits, min 0.0001) and cLLi (MaxCLL, MaxFALL) after
// the IHDR chunk of `png`. Returns false when `png` is not a PNG. Luminance fields are 0.0001 cd/m2 units.
inline bool add_hdr_chunks(std::vector<uint8_t>& png, const Params& pr, const Stats& st) {
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (png.size() < 33 || std::memcmp(png.data(), sig, 8) != 0 || std::memcmp(png.data() + 12, "IHDR", 4) != 0) return false;
  std::vector<uint8_t> add, d;
  d = {9, 16, 0, 1}; chunk(add, "cICP", d);
  d.clear();
  const double prim[8] = {0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290};   // R, G, B, white point (x, y)
  for (double p : prim) put16(d, (uint32_t)std::lround(p * 50000.0));
  put32(d, (uint32_t)std::lround((double)pr.peak * 10000.0)); put32(d, 1);
  chunk(add, "mDCv", d);
  d.clear();
  put32(d, (uint32_t)std::lround((double)st.maxCll * 10000.0)); put32(d, (uint32_t)std::lround((double)st.maxFall * 10000.0));
  chunk(add, "cLLi", d);
  // Rebuild: signature, IHDR, our chunks, then the rest without the colour-space chunks WIC adds (sRGB, gAMA, cHRM, iCCP) that contradict cICP.
  std::vector<uint8_t> out(png.begin(), png.begin() + 33);
  out.insert(out.end(), add.begin(), add.end());
  size_t i = 33;
  while (i + 12 <= png.size()) {
    const size_t n = ((size_t)png[i] << 24) | ((size_t)png[i + 1] << 16) | ((size_t)png[i + 2] << 8) | png[i + 3];
    if (i + 12 + n > png.size()) return false;
    const char* t = reinterpret_cast<const char*>(png.data() + i + 4);
    const bool drop = !std::memcmp(t, "sRGB", 4) || !std::memcmp(t, "gAMA", 4) || !std::memcmp(t, "cHRM", 4) || !std::memcmp(t, "iCCP", 4) ||
                      !std::memcmp(t, "cICP", 4) || !std::memcmp(t, "mDCv", 4) || !std::memcmp(t, "cLLi", 4);
    if (!drop) out.insert(out.end(), png.begin() + (std::ptrdiff_t)i, png.begin() + (std::ptrdiff_t)(i + 12 + n));
    i += 12 + n;
  }
  png.swap(out);
  return true;
}

}  // namespace snap_hdrphoto
