#version 450
// SPDX-License-Identifier: MIT
// Bounce light, pass 2 of 3 (nvn_lighting_fx.cpp): denoise the noisy gather of lfx_gi.frag. The same filter as lfx_resolve.frag on three colour channels:
// depth-aware 5x5 bilateral blur, then temporal accumulation (camera-motion reprojection, view-distance rejection, 3x3 neighbourhood clamp, weight alpha).
// Output rgba16f: rgb bounce radiance, a view distance (the next frame's disocclusion test and the composite's distance fade).
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1) uniform sampler2D rawTex;
layout(set = 0, binding = 2) uniform sampler2D histTex;
layout(push_constant) uniform P {
    mat4 reproj;   // (ndc.xy, depth, 1) of this frame -> previous frame's clip space
    vec4 size;     // xy: render size in pixels, z: 1 when the history is usable, w: near plane (view distance = near / depth)
    vec4 q;        // x: blend weight of the new frame
} pc;
layout(location = 0) out vec4 outRes;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 mx = ivec2(pc.size.xy) - 1;
    float d = texelFetch(depthTex, p, 0).r;
    if (d <= 0.0) { outRes = vec4(0.0, 0.0, 0.0, 1e6); return; }
    float wC = pc.size.w / d;
    vec3 cur = texelFetch(rawTex, p, 0).rgb;
    vec3 acc = vec3(0.0); float wsum = 0.0;
    vec3 nmin = cur, nmax = cur;
    for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) {
        ivec2 q = clamp(p + ivec2(x, y), ivec2(0), mx);
        float sd = texelFetch(depthTex, q, 0).r;
        if (sd <= 0.0) continue;
        float ws = pc.size.w / sd;
        float wd = max(0.0, 1.0 - abs(ws - wC) / (0.03 * wC + 0.04));
        float wt = exp(-0.12 * float(x * x + y * y)) * wd;
        vec3 v = texelFetch(rawTex, q, 0).rgb;
        if (abs(x) <= 1 && abs(y) <= 1) { nmin = min(nmin, v); nmax = max(nmax, v); }
        acc += v * wt; wsum += wt;
    }
    vec3 spat = wsum > 1e-4 ? acc / wsum : cur;
    vec3 res = spat;
    if (pc.size.z > 0.5) {
        vec4 pcl = pc.reproj * vec4((vec2(p) + 0.5) / pc.size.xy * vec2(2.0, -2.0) + vec2(-1.0, 1.0), d, 1.0);
        if (pcl.w > 1e-4) {
            vec2 uv = pcl.xy / pcl.w * vec2(0.5, -0.5) + 0.5;
            if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)))) {
                vec4 hist = texture(histTex, uv);
                if (abs(hist.a - pcl.w) < 0.05 * pcl.w + 0.1) {
                    vec3 hv = clamp(hist.rgb, nmin - 0.03, nmax + 0.03);
                    res = mix(hv, spat, pc.q.x);
                }
            }
        }
    }
    outRes = vec4(res, wC);
}
