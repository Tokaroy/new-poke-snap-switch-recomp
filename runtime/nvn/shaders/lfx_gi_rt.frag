#version 460
// SPDX-License-Identifier: MIT
// RT bounce light (SNAP_RT_GI), pass 1 of the bounce-light chain (nvn_lighting_fx.cpp), ray traced variant of lfx_gi.frag. One cosine-weighted ray per sample from the
// pixel's world position into the captured scene TLAS (nvn_rt.cpp, built at the previous frame's tonemap draw: opaque geometry, world space). The radiance a hit
// returns is the mean lit colour of the instance it landed on (the per-instance cache ReflectionPass/GiPass keep up to date, indexed by instanceCustomIndex),
// faded with distance. Unlike the screen-space gather this sees what is off screen or hidden behind the receiver: the wall behind the camera, the underside of a ledge.
// Output rgb = gathered radiance (noisy by design: lfx_gires.frag does the same bilateral + temporal filtering it does for the screen-space gather), a unused.
// World positions and the normal (from depth) are as lfx_gi.frag; the TLAS shares that world space.
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(buffer_reference, std430) readonly buffer AvgBuf { vec4 v[]; };
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1) uniform sampler2D colorTex;
layout(push_constant) uniform P {
    mat4 ivp;     // (ndc.xy, depth, 1) -> homogeneous world, w = 1 / clip.w
    vec4 size;    // xy: render size in pixels, zw: scene viewport jitter in pixels
    vec4 q;       // x: gather radius (world units), y: ray count, z: frame counter, w: radiance clamp
    uvec2 tlas;   // the scene TLAS (device address)
    uvec2 avg;    // per-instance mean colour (device address), vec4 per instance id
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

    float R = pc.q.x;
    float fr = fract(pc.q.z * 0.6180339887);
    float n1 = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + fr);
    float n2 = fract(0.7548776662 * gl_FragCoord.x + 0.5698402910 * gl_FragCoord.y + fr * 3.7);
    vec3 t = normalize(abs(n.y) < 0.9 ? cross(n, vec3(0.0, 1.0, 0.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
    vec3 b = cross(n, t);
    int N = int(pc.q.y);
    vec3 acc = vec3(0.0);
    AvgBuf avg = AvgBuf(pc.avg);
    vec3 org = pC + n * (0.02 + 0.0015 * wC);   // lift off the receiver (its own triangle is in the TLAS; depth is quantized)
    for (int k = 0; k < N; k++) {
        // golden-ratio low-discrepancy pair per ray, cosine-weighted hemisphere around the normal
        float u1 = fract(n1 + float(k) * 0.7548776662), u2 = fract(n2 + float(k) * 0.5698402910);
        float r = sqrt(u1), ph = 2.0 * PI * u2;
        vec3 dir = normalize(t * (r * cos(ph)) + b * (r * sin(ph)) + n * sqrt(max(1.0 - u1, 0.0)));
        rayQueryEXT rq;
        rayQueryInitializeEXT(rq, accelerationStructureEXT(pc.tlas), gl_RayFlagsOpaqueEXT, 0xFF, org, 0.0, dir, R);
        while (rayQueryProceedEXT(rq)) {}
        if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) continue;   // nothing within the radius: the baked ambient already covers it
        float tt = rayQueryGetIntersectionTEXT(rq, true);
        vec4 c = avg.v[rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)];
        if (c.a < 0.5) continue;                                                                           // an instance whose colour is not known yet
        float fall = 1.0 - tt / R;
        acc += min(c.rgb, vec3(pc.q.w)) * (fall * fall);
    }
    outV = vec4(acc / float(N), 0.0);
}
