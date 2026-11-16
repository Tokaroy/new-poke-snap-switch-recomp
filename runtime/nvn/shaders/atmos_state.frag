#version 450
// SPDX-License-Identifier: MIT
// Atmosphere, scene statistics pass B (nvn_atmos.cpp): reduces the 16x9 tiles to the smoothed state, a 2x1 RGBA32F image kept between frames.
//   texel 0: rgb = fog colour (sky colour near the horizon, smoothed), a = adapted log2 luminance (eye adaptation)
//   texel 1: r = this frame's log2 luminance, g = brightest sample, b = sky weight, a = ground height under the view (smoothed)
layout(set = 0, binding = 0) uniform sampler2D tile0;
layout(set = 0, binding = 1) uniform sampler2D tile1;
layout(set = 0, binding = 2) uniform sampler2D tile2;
layout(set = 0, binding = 3) uniform sampler2D prevState;
//#include atmos_common
layout(location = 0) out vec4 outS;

void main() {
    vec4 sky = vec4(0.0), lg = vec4(0.0), mx = vec4(0.0);
    for (int y = 0; y < 9; y++) for (int x = 0; x < 16; x++) {
        sky += texelFetch(tile0, ivec2(x, y), 0);
        lg += texelFetch(tile1, ivec2(x, y), 0);
        vec4 m = texelFetch(tile2, ivec2(x, y), 0);
        mx.x = max(mx.x, m.x); mx.zw += m.zw;
    }
    float logAvg = lg.y > 1e-4 ? lg.x / lg.y : -3.0;
    bool hist = u.cam.w > 0.5;
    vec4 p0 = texelFetch(prevState, ivec2(0, 0), 0), p1 = texelFetch(prevState, ivec2(1, 0), 0);
    float aExp = hist ? 1.0 - exp(-u.cam.z / max(u.expoB.x, 0.05)) : 1.0;
    float aFog = hist ? 1.0 - exp(-u.cam.z / 0.8) : 1.0;
    float adapted = mix(hist ? p0.a : logAvg, logAvg, aExp);
    vec3 skyT = sky.a > 100.0 ? sky.rgb / sky.a : vec3(0.55, 0.62, 0.72) * (exp2(logAvg) * 0.9);   // no sky in view (cave, canopy): a bluish haze a little dimmer than the scene
    vec3 fogC = mix(hist ? p0.rgb : skyT, skyT, aFog);
    float gy0 = hist ? p1.a : u.eye.y - 2.0;
    float gy = lg.w > 0.5 ? mix(gy0, lg.z / lg.w, aFog) : gy0;
    if (int(gl_FragCoord.x) == 0) outS = vec4(fogC, adapted);
    else outS = vec4(logAvg, mx.x, sky.a, gy);
}
