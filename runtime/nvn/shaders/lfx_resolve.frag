#version 450
// SPDX-License-Identifier: MIT
// Enhanced lighting, pass 2 of 3 (nvn_lighting_fx.cpp): denoise the noisy AO / contact-shadow image of lfx_ao.frag. A depth-aware 5x5 bilateral blur, then
// temporal accumulation: the previous result is reprojected with the camera motion (reproj = previous VP * inverse current VP), rejected where the view
// distance it stored disagrees, clamped to the 3x3 range of this frame's raw values, and blended with weight alpha. Output rgba16f: r AO, g contact,
// b view distance (the next frame's disocclusion test and the composite's distance fade), a unused.
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
    if (d <= 0.0) { outRes = vec4(1.0, 1.0, 1e6, 1.0); return; }
    float wC = pc.size.w / d;
    vec2 cur = texelFetch(rawTex, p, 0).rg;
    vec2 acc = vec2(0.0); float wsum = 0.0;
    vec2 nmin = cur, nmax = cur;
    for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) {
        ivec2 q = clamp(p + ivec2(x, y), ivec2(0), mx);
        float sd = texelFetch(depthTex, q, 0).r;
        if (sd <= 0.0) continue;
        float ws = pc.size.w / sd;
        float wd = max(0.0, 1.0 - abs(ws - wC) / (0.03 * wC + 0.04));
        float wg = exp(-0.25 * float(x * x + y * y));
        vec2 v = texelFetch(rawTex, q, 0).rg;
        if (abs(x) <= 1 && abs(y) <= 1) { nmin = min(nmin, v); nmax = max(nmax, v); }
        float wt = wg * wd;
        acc += v * wt; wsum += wt;
    }
    vec2 spat = wsum > 1e-4 ? acc / wsum : cur;
    vec2 res = spat;
    if (pc.size.z > 0.5) {
        vec4 pcl = pc.reproj * vec4((vec2(p) + 0.5) / pc.size.xy * vec2(2.0, -2.0) + vec2(-1.0, 1.0), d, 1.0);
        if (pcl.w > 1e-4) {
            vec2 uv = pcl.xy / pcl.w * vec2(0.5, -0.5) + 0.5;
            if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)))) {
                vec4 hist = texture(histTex, uv);
                if (abs(hist.b - pcl.w) < 0.05 * pcl.w + 0.1) {
                    vec2 hv = clamp(hist.rg, nmin - 0.03, nmax + 0.03);
                    res = mix(hv, spat, pc.q.x);
                }
            }
        }
    }
    outRes = vec4(res, wC, 1.0);
}
