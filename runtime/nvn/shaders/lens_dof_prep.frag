#version 450
#extension GL_GOOGLE_include_directive : require
#include "lens_common.glsl"
// SPDX-License-Identifier: MIT
// Bokeh depth of field, step 1 (nvn_lens_fx.cpp): the scene colour and the circle of confusion (CoC) at 1/ds of the render size. Each low-resolution pixel
// averages four taps spread over its ds x ds block (colour weighted against fireflies; CoC averaged). The CoC is signed: positive behind the focus, negative in front.
// Thin-lens form: coc = K * (1/focus - 1/z), in pixels at the render size (K carries the zoom, strength and render height), with a small dead zone around the
// focus so the subject and what sits at its distance stay sharp.
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1) uniform sampler2D colorTex;   // linear
layout(set = 0, binding = 3) uniform sampler2D focusTex;
layout(push_constant) uniform P {
    vec4 dp;   // x near, y far, z depth mode, w render height in pixels
    vec4 sz;   // xy low-resolution size, zw render size
    vec4 cc;   // x K (pixels * metres), y max far CoC (pixels), z dead zone (relative), w near field scale
    vec4 ms;   // x downscale factor ds
} pc;
layout(location = 0) out vec4 outPrep;

float cocOf(float z, float F) {
    float inv = 1.0 / F - 1.0 / z;
    float tol = pc.cc.z / F;
    inv = sign(inv) * max(abs(inv) - tol, 0.0);
    float c = pc.cc.x * inv;
    return c >= 0.0 ? min(c, pc.cc.y) : max(c * pc.cc.w, -pc.cc.y * pc.cc.w);
}

void main() {
    float F = texelFetch(focusTex, ivec2(0), 0).x;
    float ds = pc.ms.x;
    vec2 center = gl_FragCoord.xy * ds;   // centre of this pixel's block in render pixels
    vec3 acc = vec3(0.0); float wsum = 0.0, coc = 0.0;
    for (int i = 0; i < 4; i++) {
        vec2 o = vec2(float(i & 1), float(i >> 1)) - 0.5;
        vec2 p = center + o * 0.5 * ds;          // taps at +-0.25 ds around the block centre
        ivec2 ip = clamp(ivec2(p), ivec2(0), ivec2(pc.sz.zw) - 1);
        float z = linZ(texelFetch(depthTex, ip, 0).r, pc.dp.x, pc.dp.y, pc.dp.z);
        coc += cocOf(z, F) * 0.25;
        vec3 col = min(textureLod(colorTex, p / pc.sz.zw, 0.0).rgb, vec3(256.0));
        float w = 1.0 / (1.0 + dot(col, vec3(0.2126, 0.7152, 0.0722)));
        acc += col * w; wsum += w;
    }
    outPrep = vec4(acc / wsum, coc);
}
