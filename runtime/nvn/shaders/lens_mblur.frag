#version 450
#extension GL_GOOGLE_include_directive : require
#include "lens_common.glsl"
// SPDX-License-Identifier: MIT
// Motion blur (nvn_lens_fx.cpp). Velocity per pixel = where this pixel was in the previous frame minus where it is now, from the scene depth and one reprojection
// matrix (the previous view-projection times the inverse of this frame's; the same maths as the upscaler's motion vectors, upscale_mv.comp, without needing
// the upscaler). The pixel is averaged over 14 taps along its velocity scaled to the shutter (shutter seconds / frame seconds, set by the host), jittered by
// interleaved gradient noise. Taps that are clearly nearer than the pixel count for little, so a fast near object does not smear over what is behind it.
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1) uniform sampler2D colorTex;   // copy of the scene colour, linear
layout(push_constant) uniform P {
    vec4 r0, r1, r2, r3;   // rows of the reprojection matrix (row-major)
    vec4 sz;               // xy render size, zw this frame's viewport jitter in pixels
    vec4 dp;               // x near, y far, z depth mode, w frame index (0 = static pattern)
    vec4 mp;               // x velocity scale, y max blur length (pixels), z min blur length (pixels), w 1 = debug (velocity tint)
} pc;
layout(location = 0) out vec4 outColor;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 center = texelFetch(colorTex, p, 0);
    float d = texelFetch(depthTex, p, 0).r;
    vec2 win = vec2(p) + 0.5 - pc.sz.zw;
    vec4 ndc = vec4(win.x / pc.sz.x * 2.0 - 1.0, 1.0 - win.y / pc.sz.y * 2.0, d, 1.0);
    vec4 clip = vec4(dot(pc.r0, ndc), dot(pc.r1, ndc), dot(pc.r2, ndc), dot(pc.r3, ndc));
    vec2 v = vec2(0.0);
    if (clip.w > 1.0e-6) {
        vec2 pn = clip.xy / clip.w;
        vec2 pwin = vec2((pn.x * 0.5 + 0.5) * pc.sz.x, (0.5 - pn.y * 0.5) * pc.sz.y);
        v = pwin - win;
    }
    v *= pc.mp.x;
    float len = length(v);
    if (pc.mp.w > 0.5) { outColor = vec4(mix(center.rgb, vec3(abs(v) / max(pc.mp.y, 1.0), 0.2), clamp(len / pc.mp.y, 0.0, 1.0) * 0.8), center.a); return; }
    if (len < pc.mp.z) { outColor = center; return; }
    if (len > pc.mp.y) v *= pc.mp.y / len;
    const int N = 14;
    float zc = linZ(d, pc.dp.x, pc.dp.y, pc.dp.z);
    float jit = ign(vec2(p), pc.dp.w);
    vec3 acc = center.rgb; float ws = 1.0;
    for (int i = 0; i < N; i++) {
        float t = (float(i) + jit) / float(N) - 0.5;
        vec2 sp = vec2(p) + 0.5 + v * t;
        ivec2 ip = clamp(ivec2(sp), ivec2(0), ivec2(pc.sz.xy) - 1);
        vec3 col = textureLod(colorTex, sp / pc.sz.xy, 0.0).rgb;
        float zs = linZ(texelFetch(depthTex, ip, 0).r, pc.dp.x, pc.dp.y, pc.dp.z);
        float w = mix(0.1, 1.0, smoothstep(0.88, 1.0, zs / max(zc, 1.0e-3)));
        acc += min(col, vec3(256.0)) * w; ws += w;
    }
    outColor = vec4(acc / ws, center.a);
}
