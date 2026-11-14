#version 450
// Photo capture resamplers on the source's GUEST texel grid (runtime/nvn/nvn_capture.cpp). Each guest texel is the box average of its
// host footprint (4 bilinear taps a quarter guest texel off center). Defines: CAPTURE = sRGB encode per tap and * multiplyValue +
// addValue (CaptureCBuffer, set 0 binding 21); SINGLE = one bilinear tap (CapturePS) instead of the 6x6 Lanczos3 (CaptureLanczos3PS,
// Lanczos3PS).
layout(constant_id = 100) const float GW = 1920.0;
layout(constant_id = 101) const float GH = 1080.0;
layout(set = 2, binding = 32) uniform sampler2D Src;
#ifdef CAPTURE
layout(set = 0, binding = 21, std140) uniform C4 { vec4 v[1]; } c4;
#endif
layout(location = 1) in vec4 uv;
layout(location = 0) out vec4 o;
vec2 G = vec2(GW, GH), inv = 1.0 / G, d = 0.25 * inv;
vec3 box(vec2 c) {   // guest texel centered at c (uv)
  return 0.25 * (texture(Src, c + vec2(-d.x, -d.y)).rgb + texture(Src, c + vec2(d.x, -d.y)).rgb +
                 texture(Src, c + vec2(-d.x, d.y)).rgb + texture(Src, c + vec2(d.x, d.y)).rgb);
}
vec3 enc(vec3 c) {
#ifdef CAPTURE
  vec3 hi = exp2(log2(c) * 0.416660011) * 1.05499995 - 0.0549999997;
  return mix(hi, c * 12.9200001, lessThanEqual(c, vec3(0.00313080009)));
#else
  return c;
#endif
}
float lz(float x) {
  x = abs(x);
  if (x < 1e-5) return 1.0;
  if (x >= 3.0) return 0.0;
  float px = 3.14159274 * x;
  return 3.0 * sin(px) * sin(px / 3.0) / (px * px);
}
void main() {
  vec2 p = uv.xy * G - 0.5, b = floor(p), f = p - b;
#ifdef SINGLE
  vec2 c = (b + 0.5) * inv;
  vec3 r = enc(mix(mix(box(c), box(c + vec2(inv.x, 0.0)), f.x), mix(box(c + vec2(0.0, inv.y)), box(c + inv), f.x), f.y));
#else
  float wx[6], wy[6];
  for (int i = 0; i < 6; i++) { wx[i] = lz(float(i - 2) - f.x); wy[i] = lz(float(i - 2) - f.y); }
  vec3 acc = vec3(0.0); float ws = 0.0;
  for (int j = 0; j < 6; j++) for (int i = 0; i < 6; i++) {
    float w = wx[i] * wy[j];
    acc += enc(box((b + vec2(float(i - 2), float(j - 2)) + 0.5) * inv)) * w; ws += w;
  }
  vec3 r = acc / ws;
#endif
#ifdef CAPTURE
  r = r * c4.v[0].y + c4.v[0].x;
#endif
  o = vec4(r, 1.0);
}
