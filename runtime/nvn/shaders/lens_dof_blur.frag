#version 450
#extension GL_GOOGLE_include_directive : require
#include "lens_common.glsl"
// SPDX-License-Identifier: MIT
// Bokeh depth of field, step 2 (nvn_lens_fx.cpp): a round disk gather at 1/ds of the render size. Each pixel gathers over its own circle of confusion with a golden-angle
// (Vogel) spiral of 44 taps, so a bright spot grows into a round, evenly filled disc (the bokeh ball). A tap counts when its own circle reaches the pixel
// (coverage = saturate(|coc_tap| / ds - distance + 1)), which keeps a sharp subject from leaking into the blurred background and lets a blurred foreground
// spread its colour. Pixels that are in focus (CoC under half a texel) pass through. The spiral is rotated per pixel (and per frame when the upscaler
// jitters), so the temporal upscaler averages the sampling pattern away.
layout(set = 0, binding = 2) uniform sampler2D prepTex;   // linear
layout(push_constant) uniform P {
    vec4 sz;   // xy low-resolution size, z downscale factor ds, w frame index (0 = static pattern)
} pc;
layout(location = 0) out vec4 outBlur;

void main() {
    vec2 p = gl_FragCoord.xy;
    vec4 c0 = texelFetch(prepTex, ivec2(p), 0);
    float r0 = abs(c0.a) / pc.sz.z;
    if (r0 < 0.5) { outBlur = c0; return; }
    const int N = 44;
    float rot = ign(p, pc.sz.w) * 6.2831853;
    vec3 acc = c0.rgb; float wsum = 1.0;
    for (int i = 0; i < N; i++) {
        float r = sqrt((float(i) + 0.5) / float(N)) * r0;
        float th = float(i) * 2.3999632 + rot;
        vec2 q = p + r * vec2(cos(th), sin(th));
        vec4 s = textureLod(prepTex, q / pc.sz.xy, 0.0);
        float cover = clamp(abs(s.a) / pc.sz.z - r + 1.0, 0.0, 1.0);
        acc += min(s.rgb, vec3(256.0)) * cover; wsum += cover;
    }
    outBlur = vec4(acc / wsum, c0.a);
}
