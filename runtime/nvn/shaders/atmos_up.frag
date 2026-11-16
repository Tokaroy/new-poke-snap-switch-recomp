#version 450
// SPDX-License-Identifier: MIT
// Atmosphere, bloom chain upsample (nvn_atmos.cpp): 3x3 tent filter of the smaller level, added (blend ONE, ONE) into the next larger one.
layout(set = 0, binding = 0) uniform sampler2D srcTex;
//#include atmos_common
layout(location = 0) out vec4 outC;

void main() {
    vec2 uv = gl_FragCoord.xy / u.pass.zw;
    vec2 t = 1.0 / vec2(textureSize(srcTex, 0));
    vec3 a = textureLod(srcTex, uv + t * vec2(-1, 1), 0.0).rgb, b = textureLod(srcTex, uv + t * vec2(0, 1), 0.0).rgb, c = textureLod(srcTex, uv + t * vec2(1, 1), 0.0).rgb;
    vec3 d = textureLod(srcTex, uv + t * vec2(-1, 0), 0.0).rgb, e = textureLod(srcTex, uv, 0.0).rgb,                    f = textureLod(srcTex, uv + t * vec2(1, 0), 0.0).rgb;
    vec3 g = textureLod(srcTex, uv + t * vec2(-1, -1), 0.0).rgb, h = textureLod(srcTex, uv + t * vec2(0, -1), 0.0).rgb, i = textureLod(srcTex, uv + t * vec2(1, -1), 0.0).rgb;
    outC = vec4((e * 4.0 + (b + d + f + h) * 2.0 + (a + c + g + i)) * (1.0 / 16.0), 0.0);
}
