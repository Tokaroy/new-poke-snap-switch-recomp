#version 450
// SPDX-License-Identifier: MIT
// Atmosphere, light shafts (nvn_atmos.cpp): screen-space radial shafts. The game's dynamic shadow map holds only the near casters (no canopy), so the
// shafts come from the picture instead: from each pixel march toward the sun's screen position and count how much open sky the ray passes over.
// Sky seen through gaps in leaves and between trunks becomes shafts; solid geometry blocks them. Half resolution, jittered start,
// output rgb = in-scattered light (scene-scale 1.0, the composite multiplies by the adapted brightness), a = view depth of the pixel
// (the composite weights its 4 taps by depth).
layout(set = 0, binding = 0) uniform sampler2D depthTex;
//#include atmos_common
layout(location = 0) out vec4 outV;

const float PI = 3.14159265;

void main() {
    ivec2 hp = ivec2(gl_FragCoord.xy);
    ivec2 fullSz = ivec2(u.size.xy);
    ivec2 fp = min(hp * 2 + ivec2(1), fullSz - 1);
    float d = texelFetch(depthTex, fp, 0).r;
    float zv = 1e6;
    vec3 P = vec3(0.0);
    if (d > 0.0) P = worldAt(vec2(fp), d, zv);
    float zOut = min(zv, 60000.0);
    // view direction of this pixel (for the phase function)
    float zd;
    vec3 Pfar = worldAt(vec2(fp), 1e-6, zd);
    vec3 dir = normalize(Pfar - u.eye.xyz);
    float cs = dot(dir, u.light.xyz);                      // toward the sun: forward scattering
    float g = u.shaftB.x;
    float hg = (1.0 - g * g) / (4.0 * PI * pow(max(1.0 + g * g - 2.0 * g * cs, 1e-3), 1.5));
    float phase = hg * 4.0 * PI * 0.25 + 0.04;              // isotropic = 0.25, plus a small floor
    // sun position on screen (pixels); behind the camera gives no shafts
    vec4 c = u.vp * vec4(u.eye.xyz + u.light.xyz * 1000.0, 1.0);
    if (c.w <= 0.01 || u.cam.y < 0.5) { outV = vec4(u.dbg.x > 7.5 ? (c.w <= 0.01 ? 1.0 : 0.0) : 0.0, u.dbg.x > 7.5 && u.cam.y < 0.5 ? 1.0 : 0.0, 0.0, zOut); return; }
    vec2 ndc = c.xy / c.w;
    vec2 sunPx = vec2((ndc.x * 0.5 + 0.5) * u.size.x, (0.5 - ndc.y * 0.5) * u.size.y) + u.size.zw;
    vec2 pix = vec2(fp) + 0.5;
    vec2 toSun = sunPx - pix;
    float len = length(toSun);
    float maxLen = u.shaft.z * u.size.y;                    // fraction of the screen height
    float marchLen = min(len, maxLen);
    int N = int(u.shaft.w + 0.5);
    vec2 stepv = toSun / max(len, 1e-3) * (marchLen / float(N));
    float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + fract(u.light.w * 0.6180339887));
    vec2 q = pix + stepv * jit;
    float decay = u.shaft.y;                                // per-step falloff
    float w = 1.0, acc = 0.0, wsum = 0.0;
    for (int i = 0; i < N; i++) {
        ivec2 qi = ivec2(clamp(q, vec2(0.0), u.size.xy - 1.0));
        float sd = texelFetch(depthTex, qi, 0).r;
        float sky = isSkyDepth(sd) ? 1.0 : 0.0;
        acc += sky * w; wsum += w;
        w *= decay;
        q += stepv;
    }
    float open = acc / max(wsum, 1e-3);                     // 0..1: how much of the path toward the sun is open sky
    float pixelSky = isSkyDepth(d) ? 0.0 : 1.0;                  // the shaft is air in front of the surface: sky pixels themselves stay bright, no extra
    vec3 col = u.sunCol.rgb * (u.shaft.x * phase * open * (0.35 + 0.65 * pixelSky));
    if (u.dbg.x > 7.5) col = vec3(0.0, 0.0, open);   // debug 8: red = sun behind the camera, green = no sun, blue = open-sky fraction along the ray
    outV = vec4(col, zOut);
}
