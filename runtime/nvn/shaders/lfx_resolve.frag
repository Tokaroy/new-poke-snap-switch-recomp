#version 450
// SPDX-License-Identifier: MIT
// Enhanced lighting, pass 2 of 3 (nvn_lighting_fx.cpp): denoise the noisy AO / contact-shadow image of lfx_ao.frag. A depth-aware 5x5 bilateral blur, then
// temporal accumulation: the previous result is reprojected with the camera motion (reproj = previous VP * inverse current VP), rejected where the view
// distance it stored disagrees, clamped to the range of this frame's raw values around the pixel, and blended in. Output rgba16f: r AO, g contact,
// b view distance (the next frame's disocclusion test and the composite's distance fade), a the number of frames accumulated in this pixel.
// The blend weight is 1 / frames, never under alpha while the view moves. While the view holds still (the view matrix did not change, q.z) the
// count runs on to q.y frames and the value then stands, the history is read at the same pixel and the clamp is the soft one below: with a fixed weight of 0.1 a still picture
// kept flickering by several levels on foliage (8% of the pixels of a frozen frame moved by more than 2 of 255).
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1) uniform sampler2D rawTex;
layout(set = 0, binding = 2) uniform sampler2D histTex;
layout(push_constant) uniform P {
    mat4 reproj;   // (ndc.xy, depth, 1) of this frame -> previous frame's clip space
    vec4 size;     // xy: render size in pixels, z: 1 when the history is usable, w: near plane (view distance = near / depth)
    vec4 q;        // x: blend weight of the new frame while the view moves, y: most frames a still view accumulates, z: 1 when the view did not move
} pc;
layout(location = 0) out vec4 outRes;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 mx = ivec2(pc.size.xy) - 1;
    float d = texelFetch(depthTex, p, 0).r;
    if (d <= 0.0) { outRes = vec4(1.0, 1.0, 1e6, 1.0); return; }
    float wC = pc.size.w / d;
    vec2 cur = texelFetch(rawTex, p, 0).rg;
    vec2 acc = vec2(0.0); vec2 wsum = vec2(0.0);   // r (AO) and g (contact) have their own kernel: contact is noisier, so its taps are weighted flatter
    vec2 nmin = cur, nmax = cur, wmin = cur, wmax = cur;   // raw range of the 3x3 and of the whole 5x5
    for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) {
        ivec2 q = clamp(p + ivec2(x, y), ivec2(0), mx);
        float sd = texelFetch(depthTex, q, 0).r;
        if (sd <= 0.0) continue;
        float ws = pc.size.w / sd;
        float wd = max(0.0, 1.0 - abs(ws - wC) / (0.03 * wC + 0.04));
        float wg = exp(-0.25 * float(x * x + y * y));
        vec2 v = texelFetch(rawTex, q, 0).rg;
        if (abs(x) <= 1 && abs(y) <= 1) { nmin = min(nmin, v); nmax = max(nmax, v); }
        wmin = min(wmin, v); wmax = max(wmax, v);
        vec2 wt = vec2(wg, exp(-0.06 * float(x * x + y * y))) * wd;
        acc += v * wt; wsum += wt;
    }
    vec2 spat = vec2(wsum.x > 1e-4 ? acc.x / wsum.x : cur.x, wsum.y > 1e-4 ? acc.y / wsum.y : cur.y);
    vec2 res = spat;
    float cnt = 1.0;
    if (pc.size.z > 0.5) {
        bool still = pc.q.z > 0.5;   // the view did not move: the history is this same pixel, read without resampling
        vec2 uv = (vec2(p) + 0.5) / pc.size.xy;
        float wP = wC;               // this pixel's view distance in the previous frame
        bool ok = true;
        if (!still) {
            vec4 pcl = pc.reproj * vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), d, 1.0);
            ok = pcl.w > 1e-4;
            if (ok) {
                uv = pcl.xy / pcl.w * vec2(0.5, -0.5) + 0.5; wP = pcl.w;
                ok = all(greaterThanEqual(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)));
            }
        }
        if (ok) {
            vec4 hist = still ? texelFetch(histTex, p, 0) : texture(histTex, uv);
            if (abs(hist.b - wP) < 0.05 * wP + 0.1) {
                float moving = 1.0 / max(pc.q.x, 1e-3);
                float full = max(pc.q.y, moving);
                bool done = still && hist.a >= full;   // a still view with all its frames: the value stands, so the picture stops changing altogether
                cnt = min(max(hist.a, 1.0) + 1.0, still ? full : moving);
                vec2 hv = clamp(hist.rg, nmin - 0.03, nmax + 0.03);
                if (still) {
                    // A settled value just outside this frame's raw range is the noise (a few dark raw values among many of 1), not a change: it is kept
                    // and only the count comes down, so a real change (outside the range frame after frame) is caught up with in about 20 frames.
                    // Far outside is a change: clamped, and back to the moving rate.
                    vec2 lo = wmin - 0.03, hi = wmax + 0.03;
                    vec2 ov = max(max(lo - hist.rg, hist.rg - hi), vec2(0.0));
                    float outside = max(ov.x, ov.y);
                    hv = outside > 0.3 ? clamp(hist.rg, lo, hi) : hist.rg;
                    if (outside > 0.3) cnt = min(cnt, moving);
                    else if (outside > 0.0) cnt = max(min(cnt, moving), cnt * 0.8);
                }
                res = done && cnt >= full ? hv : mix(hv, spat, 1.0 / cnt);
            }
        }
    }
    outRes = vec4(res, wC, cnt);
}
