#version 450
// SPDX-License-Identifier: MIT
// Bounce light, pass 3 of 3 (nvn_lighting_fx.cpp): adds the denoised bounce to the linear HDR scene colour as a multiply, colour = colour * (1 + strength * bounce).
// The pixel's own colour stands in for its albedo, so a green flank picks up green light and a grey rock picks up less. The distance fade keeps far terrain
// untouched. With the debug pipeline (blend off) the bounce term replaces the picture.
layout(set = 0, binding = 0) uniform sampler2D resTex;
layout(push_constant) uniform P {
    vec4 k;      // x: strength, y: fade-out distance (view units), z: largest multiplier, w: debug view (0 off)
    vec4 size;   // xy: render size in pixels
} pc;
layout(location = 0) out vec4 outMul;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 r = texelFetch(resTex, p, 0);
    float fade = 1.0 - smoothstep(0.6 * pc.k.y, pc.k.y, r.a);
    vec2 edge = min(gl_FragCoord.xy, pc.size.xy - gl_FragCoord.xy) / (0.04 * pc.size.y);
    fade *= clamp(min(edge.x, edge.y), 0.0, 1.0);
    vec3 m = min(vec3(1.0) + pc.k.x * fade * max(r.rgb, vec3(0.0)), vec3(pc.k.z));
    if (pc.k.w > 0.5) { outMul = vec4(m - 1.0, 1.0); return; }
    outMul = vec4(m, 1.0);
}
