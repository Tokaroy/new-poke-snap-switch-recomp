// SPDX-License-Identifier: GPL-3.0-only
// Additional linking permission: see ../LICENSE.
layout(set = 0, binding = 0, rgba32f) uniform image2DArray initialSpectrum;
layout(set = 0, binding = 1, rg32f) uniform image2DArray pingImage;
layout(set = 0, binding = 2, rg32f) uniform image2DArray pongImage;
layout(set = 0, binding = 3, rgba32f) uniform image2DArray displacementImage;
layout(set = 0, binding = 4, rgba32f) uniform image2DArray normalImage;
layout(set = 0, binding = 5, rg32f) uniform image2DArray foamImage;
layout(push_constant) uniform Push {
    uvec4 grid;   // N, log2(N), cascade count, seed
    uvec4 pass;   // FFT axis, stage, source image (0 ping, 1 pong), reserved
    vec4 wind;   // feet/second, direction X, direction Z, fetch feet
    vec4 tiles;  // tile lengths feet, amplitude
    vec4 detail; // choppiness, damping feet, foam threshold, foam decay/second
    vec4 frame;  // elapsed seconds, dt, foam gain, gravity feet/second squared
} pc;
const float PI = 3.14159265358979323846;
vec2 multiplyComplex(vec2 a, vec2 b) { return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }
vec2 conjugateComplex(vec2 a) { return vec2(a.x, -a.y); }
vec2 timesI(vec2 a) { return vec2(-a.y, a.x); }
int signedBin(uint i) { return i < pc.grid.x / 2u ? int(i) : int(i) - int(pc.grid.x); }
vec2 waveVector(uvec2 p, uint cascade) { return vec2(signedBin(p.x), signedBin(p.y)) * (2.0 * PI / pc.tiles[cascade]); }
vec2 loadField(ivec3 p) { return pc.pass.z == 0u ? imageLoad(pingImage, p).xy : imageLoad(pongImage, p).xy; }
