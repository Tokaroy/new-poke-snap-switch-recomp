#version 450
// SPDX-License-Identifier: MIT
// Atmosphere, composite (nvn_atmos.cpp): one read-modify-write of the linear HDR scene colour for all four effects. Blend ONE / ONE_MINUS_SRC_ALPHA on the
// float scene target, so  result = src.rgb + dst * (1 - src.a)  with
//   src.a   = 1 - gain * (1 - fog)                 (gain = eye adaptation, fog = height fog amount; may be negative: the target is RGBA16F, nothing clamps)
//   src.rgb = gain * (fogColour * fog + shafts + bloom)
// i.e. result = gain * (scene * (1 - fog) + fogColour * fog + shafts + bloom).
layout(set = 0, binding = 0) uniform sampler2D depthTex;    // nearest
layout(set = 0, binding = 1) uniform sampler2D stateTex;    // nearest, 2x1
layout(set = 0, binding = 2) uniform sampler2D shaftTex;    // nearest (half resolution, a = view depth)
layout(set = 0, binding = 3) uniform sampler2D bloomTex;    // linear (chain level 0)
//#include atmos_common
layout(location = 0) out vec4 outMul;

float fogAmount(vec3 P, float groundY) {
    vec3 ray = P - u.eye.xyz;
    float t = length(ray);
    if (t < 1e-3) return 0.0;
    vec3 dir = ray / t;
    float tt = max(t - u.fogB.x, 0.0);
    float b = max(u.fog.y, 1e-4);
    float h0 = u.eye.y - (groundY + u.fog.z);
    float k = b * dir.y;
    float base = u.fog.x * exp(clamp(-b * h0, -30.0, 30.0));
    float integral = abs(k * tt) < 1e-4 ? base * tt : base * (1.0 - exp(clamp(-k * tt, -30.0, 30.0))) / k;
    return min(1.0 - exp(-max(integral, 0.0)), u.fog.w);
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float d = texelFetch(depthTex, p, 0).r;
    vec4 st0 = texelFetch(stateTex, ivec2(0, 0), 0);
    vec4 st1 = texelFetch(stateTex, ivec2(1, 0), 0);
    float gain = 1.0;
    if (u.flags.z > 0.5) gain = clamp(exp2((u.expo.x - st0.a) * u.expo.y), u.expo.z, u.expo.w);
    float zv = 1e6;
    vec3 P = vec3(0.0);
    if (d > 0.0) P = worldAt(vec2(p), d, zv);
    float fogA = 0.0;
    if (u.flags.x > 0.5 && d > 0.0) fogA = fogAmount(P, st1.a);
    vec3 add = vec3(0.0);
    if (u.flags.y > 0.5) {
        vec2 hc = (vec2(p) + 0.5) * 0.5 - 0.5;
        ivec2 b0 = ivec2(floor(hc));
        vec2 f = hc - vec2(b0);
        ivec2 hs = ivec2(u.size.xy * 0.5 + 0.5);   // half-resolution image size
        vec3 acc = vec3(0.0); float wsum = 0.0;
        for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
            ivec2 q = clamp(b0 + ivec2(i, j), ivec2(0), hs - 1);
            vec4 s = texelFetch(shaftTex, q, 0);
            float wb = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
            float wd = 1.0 / (1.0 + 12.0 * abs(s.a - zv) / max(min(s.a, zv), 0.5));
            float w = wb * wd + 1e-5;
            acc += s.rgb * w; wsum += w;
        }
        add += acc / wsum * clamp(exp2(st0.a), 0.05, 16.0);   // the shaft term is 1.0 at scene scale 1: scale it to this scene's mean brightness
    }
    if (u.flags.w > 0.5) {
        vec2 uv = (vec2(p) + 0.5) / u.size.xy;
        add += textureLod(bloomTex, uv, 0.0).rgb * u.bloom.x;
    }
    if (u.dbg.x > 0.5) {   // 1: the shaft buffer alone, 3: the bloom alone, 4: fog amount, 5: exposure gain
        vec3 v = vec3(0.0);
        if (u.dbg.x < 1.5 || u.dbg.x > 7.5) { ivec2 q = clamp(p / 2, ivec2(0), ivec2(u.size.xy * 0.5) - 1); v = texelFetch(shaftTex, q, 0).rgb; }   // 1: the shaft buffer
        else if (u.dbg.x < 3.5) v = textureLod(bloomTex, (vec2(p) + 0.5) / u.size.xy, 0.0).rgb * u.bloom.x;
        else if (u.dbg.x < 4.5) v = vec3(fogA);
        else v = vec3(gain * 0.5);
        outMul = vec4(v, 1.0);
        return;
    }
    outMul = vec4(gain * (st0.rgb * fogA + add), 1.0 - gain * (1.0 - fogA));
}
