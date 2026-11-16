#version 450
// SPDX-License-Identifier: MIT
// Atmosphere, scene statistics pass A (nvn_atmos.cpp): a 16x9 tile image, each texel integrates 8x8 sample points of its screen tile.
//   out0 (RGBA16F): rgb = horizon-weighted sky colour sum, a = sky weight
//   out1 (RGBA32F): r = sum of w * log2(luminance) (centre-weighted, sky down-weighted), g = sum of w, b = sum of ground height * wg, a = sum of wg
//   out2 (RGBA16F): r = brightest sample luminance, g = sum of luminance * w (linear mean), b = number of sky samples
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1) uniform sampler2D colTex;   // bloom chain level 1 (quarter resolution), same uv as the screen
//#include atmos_common
layout(location = 0) out vec4 out0;
layout(location = 1) out vec4 out1;
layout(location = 2) out vec4 out2;

void main() {
    ivec2 tile = ivec2(gl_FragCoord.xy);
    vec4 sky = vec4(0.0), lg = vec4(0.0), mx = vec4(0.0);
    vec2 sz = u.size.xy;
    for (int j = 0; j < 8; j++) for (int i = 0; i < 8; i++) {
        vec2 uv = (vec2(tile) + (vec2(i, j) + 0.5) / 8.0) / vec2(16.0, 9.0);
        ivec2 px = clamp(ivec2(uv * sz), ivec2(0), ivec2(sz) - 1);
        float d = texelFetch(depthTex, px, 0).r;
        vec3 col = textureLod(colTex, uv, 0.0).rgb;
        float lum = max(lumOf(col), 1e-4);
        bool isSky = isSkyDepth(d);
        vec2 c = uv - 0.5;
        float w = exp(-dot(c, c) / (2.0 * 0.35 * 0.35)) * (isSky ? 0.35 : 1.0);
        lg.x += w * log2(lum); lg.y += w;
        mx.x = max(mx.x, lum); mx.y += lum * w;
        if (isSky) {
            float ws = 0.25 + uv.y;   // lower sky = horizon colour
            sky.rgb += col * ws; sky.a += ws; mx.z += 1.0;
        } else if (uv.y > 0.5) {
            float zv;
            vec3 P = worldAt(vec2(px), d, zv);
            vec3 r = P - u.eye.xyz;
            float len = length(r);
            if (len > 1e-3 && r.y / len < -0.12) { float wg = 0.5 + (uv.y - 0.5); lg.z += P.y * wg; lg.w += wg; }
        }
    }
    out0 = sky; out1 = lg; out2 = mx;
}
