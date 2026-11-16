#version 460
// SPDX-License-Identifier: MIT
// RT ambient occlusion (SNAP_RT_AO), pass 1 of the enhanced-lighting chain (nvn_lighting_fx.cpp), ray traced variant of lfx_ao.frag. The GTAO horizon search is replaced by
// cosine-weighted short rays (default 1.5 world units) from the pixel's world position into the captured scene TLAS (nvn_rt.cpp: opaque geometry in world space, built at the
// previous frame's tonemap draw): a ray that hits something within the radius occludes, weighted by how near the hit is. The contact-shadow march is unchanged
// and shares the pass, so the output is the same: r = AO visibility, g = contact-shadow visibility. Noisy by design, lfx_resolve.frag filters it as it does GTAO.
// Push block: the lfx_ao.frag block with q.zw carrying the TLAS device address (contact thickness is the 0.2 default here) and light.w = frame counter + 1024 * ray count.
#extension GL_EXT_ray_query : require
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(push_constant) uniform P {
    mat4 ivp;     // (ndc.xy, depth, 1) -> homogeneous world, w = 1 / clip.w
    vec4 dclip;   // VP * (L, 0): clip-space change per world unit toward the sun (contact shadows)
    vec4 light;   // xyz: unit world direction toward the sun, w: frame counter + 1024 * ray count
    vec4 size;    // xy: render size in pixels, zw: scene viewport jitter in pixels
    vec4 q;       // x: AO ray length (world units), y: contact ray length (0 = no contact shadows), zw: the scene TLAS device address (two uints as floats' bits)
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
    outV = vec4(1.0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    float d = texelFetch(depthTex, p, 0).r;
    if (d <= 0.0) return;   // sky / infinity (reversed Z)
    float wC, wR, wL, wU, wD;
    vec3 pC = posAt(p, wC);
    vec3 pR = posAt(p + ivec2(1, 0), wR), pL = posAt(p - ivec2(1, 0), wL);
    vec3 pD = posAt(p + ivec2(0, 1), wD), pU = posAt(p - ivec2(0, 1), wU);
    // normal from the depth: central differences on smooth surfaces, else the one-sided difference that agrees better (contact.frag)
    vec3 dx = abs(wR - wC) < abs(wC - wL) ? pR - pC : pC - pL;
    vec3 dy = abs(wD - wC) < abs(wC - wU) ? pD - pC : pC - pU;
    if (abs(wR + wL - 2.0 * wC) < 0.01 * wC) dx = 0.5 * (pR - pL);
    if (abs(wD + wU - 2.0 * wC) < 0.01 * wC) dy = 0.5 * (pD - pU);
    vec3 n = cross(dx, dy);
    float nl = length(n);
    if (!(nl > 1e-12)) return;
    n /= nl;
    vec4 hf = pc.ivp * vec4(ndcOf(vec2(p), d * 0.5), 1.0);   // farther along the pixel's ray
    vec3 V = normalize(pC - hf.xyz / hf.w);                   // toward the camera
    if (dot(n, V) < 0.0) n = -n;

    int lw = int(pc.light.w);
    float frame = float(lw & 1023);
    int rays = max(lw >> 10, 1);
    float fr = fract(frame * 0.6180339887);
    float n1 = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + fr);
    float n2 = fract(0.7548776662 * gl_FragCoord.x + 0.5698402910 * gl_FragCoord.y + fr * 3.7);

    // ---- ambient occlusion (rays) ------------------------------------------------------------------------------------------------------------------
    float ao = 1.0;
    if (pc.q.x > 0.0) {
        float R = pc.q.x;
        vec3 t = normalize(abs(n.y) < 0.9 ? cross(n, vec3(0.0, 1.0, 0.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
        vec3 b = cross(n, t);
        vec3 org = pC + n * (0.02 + 0.0015 * wC);   // lift off the receiver (its own triangle is in the TLAS; the depth is quantized)
        uvec2 tl = uvec2(floatBitsToUint(pc.q.z), floatBitsToUint(pc.q.w));
        float occ = 0.0;
        for (int k = 0; k < rays; k++) {
            float u1 = fract(n1 + float(k) * 0.7548776662), u2 = fract(n2 + float(k) * 0.5698402910);
            float r = sqrt(u1), ph = 2.0 * PI * u2;
            vec3 dir = normalize(t * (r * cos(ph)) + b * (r * sin(ph)) + n * sqrt(max(1.0 - u1, 0.0)));
            rayQueryEXT rq;
            rayQueryInitializeEXT(rq, accelerationStructureEXT(tl), gl_RayFlagsOpaqueEXT, 0xFF, org, 0.0, dir, R);
            while (rayQueryProceedEXT(rq)) {}
            if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) continue;
            float f = 1.0 - rayQueryGetIntersectionTEXT(rq, true) / R;   // a near hit occludes fully, one at the end of the ray not at all
            occ += f;
        }
        ao = clamp(1.0 - occ / float(rays), 0.0, 1.0);
    }

    // ---- contact shadow ----------------------------------------------------------------------------------------------------------------------------
    float contact = 1.0;
    if (pc.q.y > 0.0) {
        float ndl = dot(n, pc.light.xyz);
        float facing = smoothstep(0.02, 0.3, ndl);   // surfaces turned from the sun are already dark
        if (facing > 0.0) {
            float bias = 0.012 + 0.004 * wC, thick = 0.2 + 0.01 * wC;
            vec4 C0 = vec4(ndcOf(vec2(p), d) * wC, wC);
            vec4 C1 = C0 + pc.dclip * pc.q.y;
            float pixLen = (C1.w > 1e-3) ? length((C1.xy / C1.w - C0.xy / C0.w) * 0.5 * pc.size.xy) : 64.0;
            int N = int(clamp(pixLen * 0.5, 8.0, 32.0));
            float g = max(max(abs(wR - wC), abs(wC - wL)), max(abs(wD - wC), abs(wC - wU)));   // slope-scaled bias (see contact.frag)
            vec2 win0 = (C0.xy / C0.w * vec2(0.5, -0.5) + 0.5) * pc.size.xy;
            float occ = 0.0, wsum = 0.0;   // fractional occlusion: the weighted share of the ray's steps that end behind a surface (near steps count more), so a single noisy step moves it by 1/N, not by a whole level
            for (int i = 0; i < N; i++) {
                float t = (float(i) + n1) / float(N);
                float wt = 1.0 - 0.8 * t;
                vec4 C = C0 + pc.dclip * (pc.q.y * t);
                if (C.w < 1e-3) break;
                vec2 win = (C.xy / C.w * vec2(0.5, -0.5) + 0.5) * pc.size.xy;
                ivec2 tp = ivec2(floor(win + pc.size.zw));
                if (any(lessThan(tp, ivec2(0))) || any(greaterThanEqual(tp, ivec2(pc.size.xy)))) break;
                wsum += wt;
                float sd = texelFetch(depthTex, tp, 0).r;
                if (sd <= 0.0) continue;
                float sw = 1.0 / (pc.ivp * vec4(ndcOf(vec2(tp), sd), 1.0)).w;
                float diff = C.w - sw;
                if (diff > bias + 2.5 * g * length(win - win0) && diff < thick) occ += wt;
            }
            occ = wsum > 0.0 ? occ / wsum : 0.0;
            contact = 1.0 - occ * facing;
        }
    }
    outV = vec4(ao, contact, 0.0, 1.0);
}
