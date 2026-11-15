#version 450
// SPDX-License-Identifier: MIT
// Enhanced lighting, pass 1 of 3 (nvn_lighting_fx.cpp): ground-truth ambient occlusion (GTAO, Jimenez et al. 2016: horizon search in a few screen-space
// slices, cosine-weighted analytic integration against the surface normal) and, when a sun is known, the short screen-space contact-shadow march of
// contact.frag. One full-screen triangle at render resolution; output r = AO visibility, g = contact-shadow visibility (1 = lit). Both are noisy by design
// (per-pixel slice rotation and step jitter, shifted every frame): lfx_resolve.frag filters and accumulates them.
// World positions come from the scene depth (reversed infinite Z, the upscaler viewport jitter undone like upscale_mv.comp).
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(push_constant) uniform P {
    mat4 ivp;     // (ndc.xy, depth, 1) -> homogeneous world, w = 1 / clip.w
    vec4 dclip;   // VP * (L, 0): clip-space change per world unit toward the sun (contact shadows)
    vec4 light;   // xyz: unit world direction toward the sun, w: frame counter
    vec4 size;    // xy: render size in pixels, zw: scene viewport jitter in pixels
    vec4 q;       // x: AO radius (world units), y: contact ray length (0 = no contact shadows), z: contact thickness, w: AO quality (slices)
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

    float frame = pc.light.w;
    float fr = fract(frame * 0.6180339887);
    float n1 = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + fr);
    float n2 = fract(0.7548776662 * gl_FragCoord.x + 0.5698402910 * gl_FragCoord.y + fr * 3.7);

    // ---- ambient occlusion -------------------------------------------------------------------------------------------------------------------------
    float ao = 1.0;
    if (pc.q.x > 0.0) {
        // world units per pixel at this depth: how far the radius reaches on screen
        vec4 h1 = pc.ivp * vec4(ndcOf(vec2(p) + vec2(1.0, 0.0), d), 1.0), h2 = pc.ivp * vec4(ndcOf(vec2(p) + vec2(0.0, 1.0), d), 1.0);
        vec3 rightW = h1.xyz / h1.w - pC, downW = h2.xyz / h2.w - pC;
        float upp = max(length(rightW), 1e-6);
        float radius = pc.q.x;
        float radiusPix = clamp(radius / upp, 0.0, 0.1 * pc.size.y);
        if (radiusPix >= 2.0) {
            const int STEPS = 8;
            int slices = int(pc.q.w);
            float falloffRange = 0.4 * radius, falloffFrom = radius - falloffRange;
            float falloffMul = -1.0 / falloffRange, falloffAdd = falloffFrom / falloffRange + 1.0;
            float vis = 0.0;
            for (int s = 0; s < slices; s++) {
                float phi = (float(s) + n1) * PI / float(slices);
                vec2 om = vec2(cos(phi), -sin(phi));                    // pixel direction (y down)
                vec3 dir3 = normalize(rightW * om.x + downW * om.y);     // the same direction in the world
                vec3 orthoDir = dir3 - dot(dir3, V) * V;
                vec3 axis = normalize(cross(dir3, V));
                vec3 projN = n - axis * dot(n, axis);
                float projNLen = length(projN);
                if (projNLen < 1e-4) { vis += 1.0; continue; }
                float signN = sign(dot(orthoDir, projN));
                float cosN = clamp(dot(projN, V) / projNLen, 0.0, 1.0);
                float nAng = signN * acos(cosN);
                float low0 = cos(nAng + 0.5 * PI), low1 = cos(nAng - 0.5 * PI);
                float hc0 = low0, hc1 = low1;
                for (int j = 0; j < STEPS; j++) {
                    float t = (float(j) + n2) / float(STEPS);
                    float off = max(t * t * radiusPix, 1.0 + float(j));
                    for (int side = 0; side < 2; side++) {
                        vec2 sp = vec2(p) + 0.5 + (side == 0 ? om : -om) * off;
                        ivec2 ip = ivec2(floor(sp));
                        if (any(lessThan(ip, ivec2(0))) || any(greaterThanEqual(ip, ivec2(pc.size.xy)))) continue;
                        float sd = texelFetch(depthTex, ip, 0).r;
                        if (sd <= 0.0) continue;
                        vec4 hs = pc.ivp * vec4(ndcOf(vec2(ip), sd), 1.0);
                        vec3 delta = hs.xyz / hs.w - pC;
                        float dist = length(delta);
                        float shc = dot(delta, V) / max(dist, 1e-6);
                        float wgt = clamp(dist * falloffMul + falloffAdd, 0.0, 1.0);
                        if (side == 0) hc0 = max(hc0, mix(low0, shc, wgt)); else hc1 = max(hc1, mix(low1, shc, wgt));
                    }
                }
                float h0 = -acos(clamp(hc1, -1.0, 1.0)), h1a = acos(clamp(hc0, -1.0, 1.0));
                h0 = nAng + max(h0 - nAng, -0.5 * PI);
                h1a = nAng + min(h1a - nAng, 0.5 * PI);
                float sn = sin(nAng);
                float iarc0 = (cosN + 2.0 * h0 * sn - cos(2.0 * h0 - nAng)) * 0.25;
                float iarc1 = (cosN + 2.0 * h1a * sn - cos(2.0 * h1a - nAng)) * 0.25;
                vis += projNLen * (iarc0 + iarc1);
            }
            ao = clamp(vis / float(slices), 0.0, 1.0);
        }
    }

    // ---- contact shadow ----------------------------------------------------------------------------------------------------------------------------
    float contact = 1.0;
    if (pc.q.y > 0.0) {
        float ndl = dot(n, pc.light.xyz);
        float facing = smoothstep(0.02, 0.3, ndl);   // surfaces turned from the sun are already dark
        if (facing > 0.0) {
            float bias = 0.012 + 0.004 * wC, thick = pc.q.z + 0.01 * wC;
            vec4 C0 = vec4(ndcOf(vec2(p), d) * wC, wC);
            vec4 C1 = C0 + pc.dclip * pc.q.y;
            float pixLen = (C1.w > 1e-3) ? length((C1.xy / C1.w - C0.xy / C0.w) * 0.5 * pc.size.xy) : 64.0;
            int N = int(clamp(pixLen * 0.5, 8.0, 32.0));
            float g = max(max(abs(wR - wC), abs(wC - wL)), max(abs(wD - wC), abs(wC - wU)));   // slope-scaled bias (see contact.frag)
            vec2 win0 = (C0.xy / C0.w * vec2(0.5, -0.5) + 0.5) * pc.size.xy;
            float occ = 0.0, firstT = 0.0; int hits = 0;
            for (int i = 0; i < N; i++) {
                float t = (float(i) + n1) / float(N);
                vec4 C = C0 + pc.dclip * (pc.q.y * t);
                if (C.w < 1e-3) break;
                vec2 win = (C.xy / C.w * vec2(0.5, -0.5) + 0.5) * pc.size.xy;
                ivec2 tp = ivec2(floor(win + pc.size.zw));
                if (any(lessThan(tp, ivec2(0))) || any(greaterThanEqual(tp, ivec2(pc.size.xy)))) break;
                float sd = texelFetch(depthTex, tp, 0).r;
                if (sd <= 0.0) continue;
                float sw = 1.0 / (pc.ivp * vec4(ndcOf(vec2(tp), sd), 1.0)).w;
                float diff = C.w - sw;
                if (diff > bias + 2.5 * g * length(win - win0) && diff < thick) { if (hits++ == 0) firstT = t; if (hits >= 2) { occ = 1.0 - 0.75 * firstT; break; } }
            }
            contact = 1.0 - occ * facing;
        }
    }
    outV = vec4(ao, contact, 0.0, 1.0);
}
