#version 450
// SPDX-License-Identifier: MIT
// Bounce light ("SSGI"), pass 1 of 3 (nvn_lighting_fx.cpp): one bounce of indirect light gathered in screen space. For every pixel a few samples are taken
// in a disk around it (a per-pixel rotated golden-angle spiral, shifted every frame); a sample that lies inside the world-space radius and above the
// pixel's tangent plane lends its already-lit scene colour, weighted by the cosine at the receiver and a distance falloff. Grass beside a Pokemon tints its
// flank green, a red rock tints the ground next to it. Output rgb = gathered radiance (noisy by design, lfx_gires.frag filters it), a unused.
// World positions come from the scene depth (reversed infinite Z, the viewport jitter undone, as lfx_ao.frag). The scene colour is the linear HDR frame
// before the tonemap.
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1) uniform sampler2D colorTex;
layout(push_constant) uniform P {
    mat4 ivp;     // (ndc.xy, depth, 1) -> homogeneous world, w = 1 / clip.w
    vec4 size;    // xy: render size in pixels, zw: scene viewport jitter in pixels
    vec4 q;       // x: gather radius (world units), y: sample count, z: frame counter, w: radiance clamp
} pc;
layout(location = 0) out vec4 outV;

const float PI = 3.14159265;

vec3 ndcOf(vec2 pix, float d) {
    vec2 win = pix + 0.5 - pc.size.zw;
    return vec3(win / pc.size.xy * vec2(2.0, -2.0) + vec2(-1.0, 1.0), d);
}
vec3 posAt(ivec2 p, out float w) {
    p = clamp(p, ivec2(0), ivec2(pc.size.xy) - 1);
    float d = texelFetch(depthTex, p, 0).r;
    if (d <= 0.0) { w = 1e9; return vec3(0.0); }
    vec4 h = pc.ivp * vec4(ndcOf(vec2(p), d), 1.0);
    w = 1.0 / h.w;
    return h.xyz / h.w;
}

void main() {
    outV = vec4(0.0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    float d = texelFetch(depthTex, p, 0).r;
    if (d <= 0.0) return;
    float wC, wR, wL, wU, wD;
    vec3 pC = posAt(p, wC);
    vec3 pR = posAt(p + ivec2(1, 0), wR), pL = posAt(p - ivec2(1, 0), wL);
    vec3 pD = posAt(p + ivec2(0, 1), wD), pU = posAt(p - ivec2(0, 1), wU);
    vec3 dx = abs(wR - wC) < abs(wC - wL) ? pR - pC : pC - pL;
    vec3 dy = abs(wD - wC) < abs(wC - wU) ? pD - pC : pC - pU;
    if (abs(wR + wL - 2.0 * wC) < 0.01 * wC) dx = 0.5 * (pR - pL);
    if (abs(wD + wU - 2.0 * wC) < 0.01 * wC) dy = 0.5 * (pD - pU);
    vec3 n = cross(dx, dy);
    float nl = length(n);
    if (!(nl > 1e-12)) return;
    n /= nl;
    vec4 hf = pc.ivp * vec4(ndcOf(vec2(p), d * 0.5), 1.0);
    vec3 V = normalize(pC - hf.xyz / hf.w);
    if (dot(n, V) < 0.0) n = -n;

    vec4 h1 = pc.ivp * vec4(ndcOf(vec2(p) + vec2(1.0, 0.0), d), 1.0);
    float upp = max(length(h1.xyz / h1.w - pC), 1e-6);   // world units per pixel here
    float R = pc.q.x;
    float radiusPix = clamp(R / upp, 0.0, 0.18 * pc.size.y);
    if (radiusPix < 3.0) return;

    float fr = fract(pc.q.z * 0.6180339887);
    float n1 = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + fr);
    float n2 = fract(0.7548776662 * gl_FragCoord.x + 0.5698402910 * gl_FragCoord.y + fr * 3.7);
    int N = int(pc.q.y);
    vec3 acc = vec3(0.0);
    for (int k = 0; k < N; k++) {
        float a = (float(k) + n1) * 2.39996323 + n2 * 2.0 * PI;
        float r = sqrt((float(k) + n2) / float(N)) * radiusPix;
        r = max(r, 2.0);
        ivec2 ip = ivec2(floor(vec2(p) + 0.5 + vec2(cos(a), sin(a)) * r));
        if (any(lessThan(ip, ivec2(0))) || any(greaterThanEqual(ip, ivec2(pc.size.xy)))) continue;
        float sd = texelFetch(depthTex, ip, 0).r;
        if (sd <= 0.0) continue;
        vec4 hs = pc.ivp * vec4(ndcOf(vec2(ip), sd), 1.0);
        vec3 delta = hs.xyz / hs.w - pC;
        float dist = length(delta);
        if (dist < 0.02 || dist > R) continue;
        vec3 dir = delta / dist;
        float cosR = dot(n, dir);
        if (cosR < 0.15) continue;                       // below or along the receiver's tangent plane: it does not light this pixel
        float fall = 1.0 - dist / R;
        vec3 c = min(texelFetch(colorTex, ip, 0).rgb, vec3(pc.q.w));
        acc += c * (cosR * fall * fall);
    }
    outV = vec4(acc / float(N), 0.0);
}
