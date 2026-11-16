#version 460
// SPDX-License-Identifier: MIT
// Water effects (nvn_ssr.cpp): screen-space reflections, caustics and shoreline foam, one full-screen pass run right after the half-resolution water is
// composited into the scene. Project code, ours. The game draws its sea in a half-resolution pass (960x540 colour + depth); nvn_ssr.cpp makes those draws write
// the half-resolution depth, copies it before and after, and restores it. Reads: the full-resolution scene depth (what is behind the sea), the half-resolution
// depth before and after the water (a pixel is water when its half-resolution depth changed; the changed depth is the water surface) and the scene colour
// copied before the composite (what is reflected). Output is blended as  dst = src.rgb + dst.rgb * src.a  (colour attachment, alpha untouched):
//   W' = g * mix(mix(W, ssr, kS), foam, kF)   with   src.rgb = g * ((1 - kF) * kS * ssr + kF * foam),  src.a = g * (1 - kF) * (1 - kS).
// Reversed infinite Z (the course): depth d = near / w, w = view distance along the camera axis = clip.w.
// RT_REFL (compiled as a second module, used only when SNAP_RT_REFLECT is on and the ray tracing device is up): a reflection ray the screen-space march misses is
// traced into the captured scene TLAS (nvn_rt.cpp); the hit is shaded from the scene colour when it is visible on screen, else from that instance's cached
// colour (rt_acc.comp). A ray that leaves the scene (sky) keeps the game's own cubemap.
#ifdef RT_REFL
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(buffer_reference, std430) readonly buffer AvgBuf { vec4 v[]; };
#endif
layout(set = 0, binding = 0) uniform sampler2D depthFull;   // full-resolution scene depth (no water in it)
layout(set = 0, binding = 1) uniform sampler2D hdPost;      // half-resolution depth after the water draws (water surface where it changed)
layout(set = 0, binding = 2) uniform sampler2D colPre;      // scene colour copy before the water is composited (what is reflected)
layout(set = 0, binding = 3) uniform sampler2D hdPre;       // half-resolution depth before the first water draw
layout(push_constant) uniform P {
    mat4 vp;      // clip = vp * (world, 1)
    vec4 size;    // xy: render size in pixels, zw: scene viewport jitter in pixels
    vec4 k;       // x: near, y: time (seconds), z: flags (1 ssr, 2 caustics, 4 foam, 8 debug mask, 16 debug h, 32 debug depth, 256 rt reflections, 512 debug rt classes), w: ripple amount
    vec4 s;       // x: ssr strength, y: caustics strength, z: foam strength, w: ssr max distance (world units)
    uvec2 tlas;   // RT_REFL: the scene TLAS (device address)
    uvec2 avg;    // RT_REFL: the per-instance colour cache (device address)
} pc;
layout(location = 0) out vec4 outCol;

mat3 gMinv;     // inverse of the xyz of vp rows 0, 1 and 3
vec3 gOff;      // (vp[0].w, vp[1].w, vp[3].w)
vec3 gEye;

mat3 inv3(mat3 m) {   // m given by columns
    vec3 a = m[0], b = m[1], c = m[2];
    vec3 r0 = cross(b, c), r1 = cross(c, a), r2 = cross(a, b);
    float det = dot(a, r0);
    return transpose(mat3(r0, r1, r2)) / det;
}
vec3 ndcOf(vec2 pix, float d) {
    vec2 win = pix + 0.5 - pc.size.zw;
    return vec3(win / pc.size.xy * vec2(2.0, -2.0) + vec2(-1.0, 1.0), d);
}
vec3 worldAt(vec3 ndc, float w) {   // clip.x = ndc.x * w, clip.y = ndc.y * w, clip.w = w
    return gMinv * (vec3(ndc.x * w, ndc.y * w, w) - gOff);
}
float hash21(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }
vec2 hash22(vec2 p) { float n = hash21(p); return vec2(n, hash21(p + n + 19.19)); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + vec2(1, 0)), f.x), mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), f.x), f.y);
}
float fbm(vec2 p) { return 0.5 * vnoise(p) + 0.25 * vnoise(p * 2.03 + 7.1) + 0.125 * vnoise(p * 4.1 + 3.7); }
// animated cellular net: distance gap between the two nearest feature points (small along cell borders)
float cellGap(vec2 p, float t) {
    vec2 ip = floor(p), fp = fract(p);
    float d1 = 8.0, d2 = 8.0;
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
        vec2 g = vec2(x, y);
        vec2 o = 0.5 + 0.5 * sin(t + 6.2831 * hash22(ip + g));
        vec2 r = g + o - fp;
        float d = dot(r, r);
        if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) d2 = d;
    }
    return sqrt(d2) - sqrt(d1);
}
float causticPattern(vec2 xz, float t) {
    float a = 1.0 - smoothstep(0.0, 0.28, cellGap(xz * 0.9 + vec2(t * 0.05, 0.0), t * 0.9));
    float b = 1.0 - smoothstep(0.0, 0.28, cellGap(mat2(0.8, -0.6, 0.6, 0.8) * xz * 1.35 + vec2(3.7, t * -0.04), t * 1.1 + 2.0));
    return pow(a * 0.6 + b * 0.6, 1.6);
}
vec3 rippleNormal(vec2 xz, float t, float amp) {
    vec2 g = vec2(0.0);
    for (int i = 0; i < 4; i++) {
        float fi = float(i);
        vec2 dir = vec2(sin(fi * 2.1 + 0.3), cos(fi * 1.7 + 1.1));
        float f = 1.9 + 1.45 * fi;
        g += dir * f * cos(dot(xz, dir) * f + t * (0.8 + 0.37 * fi)) / (1.0 + 0.7 * fi);
    }
    g += (vec2(vnoise(xz * 3.1 + t * 0.3), vnoise(xz * 3.1 + 11.0 - t * 0.27)) - 0.5) * 4.0;
    return normalize(vec3(-g.x * amp, 1.0, -g.y * amp));
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    const int flags = int(pc.k.z);
    outCol = vec4(0.0, 0.0, 0.0, 1.0);
    ivec2 hp = min(p / 2, textureSize(hdPost, 0) - 1);
    float dPost = texelFetch(hdPost, hp, 0).r, hPre = texelFetch(hdPre, hp, 0).r;   // water surface depth, half-resolution
    float dPre = texelFetch(depthFull, p, 0).r;                                      // what is behind / in front of it
    if ((flags & 64) != 0) { float nr = pc.k.x; outCol = vec4(clamp(nr / max(hPre, 1e-9) / 150.0, 0.0, 1.0), clamp(nr / max(dPost, 1e-9) / 150.0, 0.0, 1.0), clamp(nr / max(dPre, 1e-9) / 150.0, 0.0, 1.0), 0.0); return; }   // debug 4: view distance /150 of hdPre (R), hdPost (G), full depth (B)
    if ((flags & 32) != 0) { bool chg = abs(dPost - hPre) > 1e-6 + 1e-4 * hPre; outCol = vec4(chg ? 1.0 : 0.0, dPre < dPost ? 1.0 : 0.0, dPost > 0.0 ? 1.0 : 0.0, 0.0); return; }   // debug 3: R depth changed by the water, G water in front of the scene, B water depth present
    const bool dbg5 = (flags & 128) != 0;
    if (!dbg5 && (dPost <= 0.0 || abs(dPost - hPre) <= 1e-6 + 1e-4 * hPre || dPre >= dPost)) discard;   // not water, or an opaque surface in front of it
    if ((flags & 8) != 0) { outCol = vec4(1.0, 0.0, 1.0, 0.0); return; }     // debug: the water mask
    float near = pc.k.x, time = pc.k.y;
    vec3 r0 = vec3(pc.vp[0][0], pc.vp[1][0], pc.vp[2][0]), r1 = vec3(pc.vp[0][1], pc.vp[1][1], pc.vp[2][1]), r3 = vec3(pc.vp[0][3], pc.vp[1][3], pc.vp[2][3]);
    mat3 M = transpose(mat3(r0, r1, r3));
    gMinv = inv3(M);
    gOff = vec3(pc.vp[3][0], pc.vp[3][1], pc.vp[3][3]);
    gEye = -(gMinv * gOff);
    float wS = near / dPost;                       // view distance of the water surface at this pixel
    vec3 ndcP = ndcOf(vec2(p), dPost);
    vec3 Pw = worldAt(ndcP, wS);
    vec3 V = normalize(gEye - Pw);
    float kS = 0.0, kF = 0.0, g = 1.0; vec3 ssr = vec3(0.0), foam = vec3(0.0);
    // height of the water surface above the geometry seen through it (infinite where nothing is behind)
    float h = 1e9;
    vec3 Pb = Pw;
    if (dPre > 0.0) { Pb = worldAt(ndcOf(vec2(p), dPre), near / dPre); h = Pw.y - Pb.y; }
    if (dbg5) { outCol = vec4(fract(Pb.y * 0.5), fract(Pw.y * 0.5), (dPost > 0.0 && abs(dPost - hPre) > 1e-6 + 1e-4 * hPre) ? 1.0 : 0.0, 0.0); return; }   // debug 5: world height of the full depth (R) and of the half-res water depth (G), mask (B)
    if ((flags & 16) != 0) { outCol = vec4(vec3(clamp(h, 0.0, 4.0) * 0.25), 0.0); return; }   // debug: water height above what is behind it, 0..4

    if ((flags & 1) != 0) {
        vec3 N = rippleNormal(Pw.xz, time, pc.k.w);
        vec3 R = reflect(-V, N);
        R.y = max(R.y, 0.03); R = normalize(R);
        float cosT = clamp(dot(V, vec3(0.0, 1.0, 0.0)), 0.0, 1.0);
        float F = 0.02 + 0.98 * pow(1.0 - cosT, 5.0);
        F = clamp(F * 1.6 + 0.10, 0.0, 1.0);
        const int STEPS = 40;
        float maxD = pc.s.w;
        float t = 0.04 + 0.004 * wS, tPrev = t;
        bool hit = false; vec2 huv = vec2(0.0); float conf = 0.0;
        float thick0 = 0.35;
        for (int i = 0; i < STEPS; i++) {
            vec3 Pr = Pw + R * t;
            vec4 c = pc.vp * vec4(Pr, 1.0);
            if (c.w <= near) break;
            vec2 ndc = c.xy / c.w;
            vec2 uv = ndc * vec2(0.5, -0.5) + 0.5 + pc.size.zw / pc.size.xy;
            if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break;
            float dS = textureLod(depthFull, uv, 0.0).r;
            if (dS > 0.0) {
                float wRay = c.w, wSc = near / dS;
                float dd = wRay - wSc;                   // > 0: the ray is behind the visible surface
                if (dd > 0.0 && dd < thick0 + 0.05 * wRay) {
                    {
                        // refine between the previous and this step
                        float lo = tPrev, hi = t;
                        for (int j = 0; j < 5; j++) {
                            float mid = 0.5 * (lo + hi);
                            vec4 cm = pc.vp * vec4(Pw + R * mid, 1.0);
                            vec2 um = cm.xy / cm.w * vec2(0.5, -0.5) + 0.5 + pc.size.zw / pc.size.xy;
                            float dm = textureLod(depthFull, um, 0.0).r;
                            if (dm > 0.0 && cm.w - near / dm > 0.0) hi = mid; else lo = mid;
                        }
                        vec4 ch = pc.vp * vec4(Pw + R * hi, 1.0);
                        huv = ch.xy / ch.w * vec2(0.5, -0.5) + 0.5 + pc.size.zw / pc.size.xy;
                        hit = true; conf = 1.0 - clamp(hi / maxD, 0.0, 1.0); conf *= conf;
                        break;
                    }
                }
            }
            tPrev = t;
            t += max(0.06, t * 0.16);
            if (t > maxD) break;
        }
        if (hit) {
            vec2 e = min(huv, 1.0 - huv);
            float edge = smoothstep(0.0, 0.08, min(e.x, e.y));
            ssr = textureLod(colPre, huv, 0.0).rgb;
            kS = clamp(F * pc.s.x * conf * edge, 0.0, 1.0);
            if ((flags & 512) != 0) { outCol = vec4(0.9, 0.1, 0.1, 0.0); return; }   // debug: screen-space hit
        }
#ifdef RT_REFL
        else if ((flags & 256) != 0) {   // screen-space missed (left the screen or went behind what is visible): ask the scene
            const float maxR = maxD * 3.0;
            rayQueryEXT rq;
            rayQueryInitializeEXT(rq, accelerationStructureEXT(pc.tlas), gl_RayFlagsOpaqueEXT, 0xFF, Pw + R * 0.05, 0.0, R, maxR);
            while (rayQueryProceedEXT(rq)) {}
            bool rtHit = rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
            if (rtHit) {
                const float th = 0.05 + rayQueryGetIntersectionTEXT(rq, true);
                const vec3 Ph = Pw + R * th;
                const float rtConf = pow(1.0 - clamp(th / maxR, 0.0, 1.0), 2.0);
                vec4 ch = pc.vp * vec4(Ph, 1.0);
                bool onScreen = false; vec2 uvh = vec2(0.0);
                if (ch.w > near) {
                    uvh = ch.xy / ch.w * vec2(0.5, -0.5) + 0.5 + pc.size.zw / pc.size.xy;
                    if (uvh.x > 0.0 && uvh.x < 1.0 && uvh.y > 0.0 && uvh.y < 1.0) {
                        float dS = textureLod(depthFull, uvh, 0.0).r;
                        onScreen = dS > 0.0 && abs(ch.w - near / dS) < 0.4 + 0.06 * ch.w;   // the hit point is what the camera sees there
                    }
                }
                if (onScreen) {
                    vec2 e = min(uvh, 1.0 - uvh);
                    ssr = textureLod(colPre, uvh, 0.0).rgb;
                    kS = clamp(F * pc.s.x * rtConf * smoothstep(0.0, 0.08, min(e.x, e.y)), 0.0, 1.0);
                    if ((flags & 512) != 0) { outCol = vec4(0.1, 0.9, 0.1, 0.0); return; }
                } else {
                    const uint id = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
                    const vec4 a = AvgBuf(pc.avg).v[id];
                    if (a.w > 0.0) {
                        ssr = a.rgb;                                        // the object's mean colour from when it was last on screen
                        kS = clamp(F * pc.s.x * rtConf * 0.85, 0.0, 1.0);
                        if ((flags & 512) != 0) { outCol = vec4(0.1, 0.3, 1.0, 0.0); return; }
                    }
                }
            } else if ((flags & 512) != 0) { outCol = vec4(0.9, 0.8, 0.1, 0.0); return; }   // debug: the ray left the scene (sky: the game's cubemap)
        }
#endif
    }
    if ((flags & 4) != 0 && h < 1e8) {
        float n = fbm(Pb.xz * 1.6 + vec2(time * 0.07, -time * 0.05));
        float bandW = 1.2 * (0.55 + 0.9 * n);
        float edgeF = 1.0 - smoothstep(0.02, bandW, h);
        float pulse = 0.5 + 0.5 * sin(h * 11.0 - time * 2.2 + n * 6.0);
        float lace = smoothstep(0.30, 0.75, fbm(Pb.xz * 5.0 + vec2(-time * 0.11, time * 0.08)));
        float f = edgeF * (0.55 + 0.45 * pulse) * (0.45 + 0.55 * lace);
        f = clamp(f * 1.35, 0.0, 1.0) * (1.0 - smoothstep(30.0, 80.0, wS));   // far water: depth precision makes it look thin, so no foam on the horizon line
        kF = f * pc.s.z;
        const vec3 base = texelFetch(colPre, p, 0).rgb;   // the scene is HDR: foam has to be brighter than what is under it, not just 'white'
        foam = vec3(0.93, 0.96, 1.0) * (0.7 + 1.6 * dot(base, vec3(0.299, 0.587, 0.114)));
    }
    if ((flags & 2) != 0 && h < 1e8 && h > 0.0) {
        float depthFade = exp(-h * 0.5) * (1.0 - smoothstep(2.5, 6.0, h));     // bright in shallows, gone in deep water
        float viewFade = 1.0 - smoothstep(60.0, 140.0, wS);
        float c = causticPattern(Pb.xz + 0.35 * Pb.y, time);
        g = 1.0 + pc.s.y * 1.6 * c * depthFade * viewFade * (1.0 - 0.5 * kS);
    }
    outCol = vec4(g * ((1.0 - kF) * kS * ssr + kF * foam), g * (1.0 - kF) * (1.0 - kS));
}
