#version 450
#extension GL_GOOGLE_include_directive : require
#include "lens_common.glsl"
// SPDX-License-Identifier: MIT
// Bokeh depth of field, focus distance (nvn_lens_fx.cpp): written into a 1x1 RGBA32F image, read by the later passes. The subject is whatever sits at the reticle
// (the centre of the image): the median view distance of a 5x5 spread of depth taps around it (thin foreground leaves and sky texels do not move it), smoothed
// over time in diopters (1 / distance) so the focus racks between subjects instead of jumping. x: focus distance, y: this frame's measurement, z: 1 once valid.
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 3) uniform sampler2D prevFocus;
layout(push_constant) uniform P {
    vec4 dp;   // x near, y far, z depth mode, w render height in pixels
    vec4 sz;   // xy unused, zw render size in pixels
    vec4 ms;   // x seconds since the last scene frame, y focus rate (1/s), z 1 = snap (first frame or after a gap)
} pc;
layout(location = 0) out vec4 outFocus;

void main() {
    vec2 c = pc.sz.zw * 0.5;
    float R = 0.035 * pc.sz.w;
    float z[25];
    int n = 0;
    for (int iy = -2; iy <= 2; iy++)
        for (int ix = -2; ix <= 2; ix++) {
            ivec2 p = clamp(ivec2(c + vec2(ix, iy) * R * 0.5), ivec2(0), ivec2(pc.sz.zw) - 1);
            float d = texelFetch(depthTex, p, 0).r;
            if (d > 0.0) z[n++] = linZ(d, pc.dp.x, pc.dp.y, pc.dp.z);
        }
    float m = 5000.0;   // nothing but sky under the reticle: focus far away
    if (n >= 6) {
        for (int i = 1; i < n; i++) {   // insertion sort of the first n values
            float v = z[i]; int j = i - 1;
            while (j >= 0 && z[j] > v) { z[j + 1] = z[j]; j--; }
            z[j + 1] = v;
        }
        m = z[n / 2];
    }
    vec4 prev = texelFetch(prevFocus, ivec2(0), 0);
    float f = m;
    if (pc.ms.z < 0.5 && prev.z > 0.5) {
        float a = 1.0 - exp(-pc.ms.x * pc.ms.y);
        f = 1.0 / mix(1.0 / max(prev.x, 0.05), 1.0 / max(m, 0.05), a);
    }
    outFocus = vec4(f, m, 1.0, 0.0);
}
