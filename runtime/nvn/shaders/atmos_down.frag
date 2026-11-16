#version 450
// SPDX-License-Identifier: MIT
// Atmosphere, bloom chain downsample (nvn_atmos.cpp): 13-tap filter (Jimenez 2014, the energy-conserving "no shimmer" kernel, own implementation).
// The first level also applies the highlight prefilter (soft knee on the brightest channel) and Karis-weighted groups so a single sparkle cannot flicker.
layout(set = 0, binding = 0) uniform sampler2D srcTex;
layout(set = 0, binding = 1) uniform sampler2D stateTex;   // last frame's scene statistics (texel 0 a = adapted log2 luminance)
//#include atmos_common
layout(location = 0) out vec4 outC;

// The threshold and knee are multiples of the scene's adapted mean luminance, so a dim cave and a bright beach bloom the same way (the scene colour is
// linear HDR in the game's own scale, mean about 2.4 in daylight). Without history (the first frame) the daylight mean stands in.
vec3 pre(vec3 c) {
    if (u.pass.x < 0.5) return c;
    float mean = u.cam.w > 0.5 ? exp2(texelFetch(stateTex, ivec2(0, 0), 0).a) : 2.4;
    mean = clamp(mean, 0.05, 16.0);
    float thr = u.bloom.y * mean, knee = max(u.bloom.z * mean, 1e-4);
    c = min(c, vec3(64.0 * mean));
    float br = max(c.r, max(c.g, c.b));
    float soft = clamp(br - thr + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float k = max(soft, br - thr) / max(br, 1e-4);
    return c * max(k, 0.0);
}
vec3 S(vec2 uv) { return pre(textureLod(srcTex, uv, 0.0).rgb); }

void main() {
    vec2 uv = (gl_FragCoord.xy) / u.pass.zw;
    vec2 t = 1.0 / vec2(textureSize(srcTex, 0));
    vec3 a = S(uv + t * vec2(-2, -2)), b = S(uv + t * vec2(0, -2)), c = S(uv + t * vec2(2, -2));
    vec3 d = S(uv + t * vec2(-2, 0)),  e = S(uv),                    f = S(uv + t * vec2(2, 0));
    vec3 g = S(uv + t * vec2(-2, 2)),  h = S(uv + t * vec2(0, 2)),   i = S(uv + t * vec2(2, 2));
    vec3 j = S(uv + t * vec2(-1, -1)), k = S(uv + t * vec2(1, -1)), l = S(uv + t * vec2(-1, 1)), m = S(uv + t * vec2(1, 1));
    vec3 r;
    if (u.pass.x > 0.5) {
        vec3 g0 = (a + b + d + e) * 0.25, g1 = (b + c + e + f) * 0.25, g2 = (d + e + g + h) * 0.25, g3 = (e + f + h + i) * 0.25, g4 = (j + k + l + m) * 0.25;
        float w0 = 0.125 / (1.0 + lumOf(g0)), w1 = 0.125 / (1.0 + lumOf(g1)), w2 = 0.125 / (1.0 + lumOf(g2)), w3 = 0.125 / (1.0 + lumOf(g3)), w4 = 0.5 / (1.0 + lumOf(g4));
        r = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);
    } else {
        r = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    }
    outC = vec4(r, 1.0);
}
