#version 450
#extension GL_GOOGLE_include_directive : require
#include "lens_common.glsl"
// SPDX-License-Identifier: MIT
// Bokeh depth of field, step 3 (nvn_lens_fx.cpp): writes the scene colour. The full-resolution circle of confusion of each pixel decides how much of the blurred
// layer replaces the sharp one; the blurred layer is brought up from 1/ds with a depth-aware (CoC-aware) bilinear filter, so a blurred background does not pick up
// the colour of the sharp subject next to it. SNAP_FX_SPLIT is handled by the scissor of the draw.
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1) uniform sampler2D sharpTex;   // copy of the scene colour
layout(set = 0, binding = 2) uniform sampler2D blurTex;
layout(set = 0, binding = 3) uniform sampler2D focusTex;
layout(push_constant) uniform P {
    vec4 dp;   // x near, y far, z depth mode, w render height in pixels
    vec4 sz;   // xy low-resolution size, zw render size
    vec4 cc;   // x K, y max far CoC, z dead zone, w near field scale
    vec4 ms;   // x ds, y debug view (1 = CoC tint)
} pc;
layout(location = 0) out vec4 outColor;

float cocOf(float z, float F) {
    float inv = 1.0 / F - 1.0 / z;
    float tol = pc.cc.z / F;
    inv = sign(inv) * max(abs(inv) - tol, 0.0);
    float c = pc.cc.x * inv;
    return c >= 0.0 ? min(c, pc.cc.y) : max(c * pc.cc.w, -pc.cc.y * pc.cc.w);
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 sharp = texelFetch(sharpTex, p, 0);
    float F = texelFetch(focusTex, ivec2(0), 0).x;
    float z = linZ(texelFetch(depthTex, p, 0).r, pc.dp.x, pc.dp.y, pc.dp.z);
    float coc = cocOf(z, F);
    float ds = pc.ms.x;
    float t = smoothstep(0.4, 2.0, abs(coc) * 1080.0 / pc.dp.w);
    if (pc.ms.y > 0.5) {   // debug: the focus plane stays clear, behind it red, in front blue; a cyan dot marks the reticle
        vec3 tint = coc >= 0.0 ? vec3(1.0, 0.2, 0.1) : vec3(0.1, 0.4, 1.0);
        vec3 o = mix(sharp.rgb, tint * (0.2 + abs(coc) / max(pc.cc.y, 1.0)), 0.6 * t);
        if (length(gl_FragCoord.xy - pc.sz.zw * 0.5) < 4.0 * pc.dp.w / 1080.0) o = vec3(0.0, 1.0, 1.0);
        outColor = vec4(o, sharp.a);
        return;
    }
    if (t <= 0.0) { outColor = sharp; return; }
    vec2 lp = (vec2(p) + 0.5) / ds - 0.5;
    ivec2 b = ivec2(floor(lp));
    vec2 f = lp - vec2(b);
    vec3 acc = vec3(0.0); float ws = 0.0;
    for (int i = 0; i < 4; i++) {
        ivec2 o = ivec2(i & 1, i >> 1);
        ivec2 q = clamp(b + o, ivec2(0), ivec2(pc.sz.xy) - 1);
        vec4 s = texelFetch(blurTex, q, 0);
        float wb = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        float wc = 1.0 / (0.5 + abs(s.a - coc) / ds);
        float w = wb * wc;
        acc += s.rgb * w; ws += w;
    }
    vec3 blur = acc / max(ws, 1.0e-6);
    outColor = vec4(mix(sharp.rgb, blur, t), sharp.a);
}
