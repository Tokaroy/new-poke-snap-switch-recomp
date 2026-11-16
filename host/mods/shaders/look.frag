#version 450
// Final-output look pass (host/mods/look.cpp): SNAP_LOOK tint plus the Accessibility colour-vision correction (hle/access.h). Our F1 menu is drawn after
// this pass, and photos / Evaluation read the game's own render targets, so neither sees any of it.
layout(set=0,binding=0) uniform sampler2D tex;   // the frame, with a mip chain down to 1x1 (the chain is only built while Reduce flashing is on)
layout(set=0,binding=1) uniform sampler2D st;    // 1x1 flash-limiter state written by look_state.frag: .y = this frame's gain
layout(push_constant) uniform P {
  vec4 gain;   // rgb gain
  vec4 p;      // saturation, contrast, look intensity, srgb flag
  vec4 q;      // cvd type (0 off, 1 protan, 2 deutan, 3 tritan), cvd strength 0..1, 0, flash limiter on
} pc;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 o;

// Machado, Oliveira, Fernandes 2009, severity 1.0, linear RGB, row-major (GLSL mat3 is column-major: written transposed).
const mat3 kProtan = mat3(0.152286, 0.114503, -0.003882,   1.052583, 0.786281, -0.048116,   -0.204868, 0.099216, 1.051998);
const mat3 kDeutan = mat3(0.367322, 0.280085, -0.011820,   0.860646, 0.672501, 0.042940,   -0.227968, 0.047413, 0.968881);
const mat3 kTritan = mat3(1.255528, -0.078411, 0.004733,   -0.076749, 0.930809, 0.691367,   -0.178779, 0.147602, 0.303900);

// Daltonize: simulate the deficiency, take what was lost, and move it into the channels the viewer still separates.
vec3 daltonize(vec3 lin, int type, float strength) {
  mat3 m = type == 1 ? kProtan : (type == 2 ? kDeutan : kTritan);
  vec3 sim = m * lin;
  vec3 e = lin - sim;
  vec3 shift = type == 3 ? vec3(e.r + 0.7 * e.b, e.g + 0.7 * e.b, 0.0)
                         : vec3(0.0, 0.7 * e.r + e.g, 0.7 * e.r + e.b);
  vec3 fixedc = clamp(lin + shift, 0.0, 1.0);
  return mix(lin, fixedc, strength);
}

void main(){
  vec3 c = textureLod(tex, uv, 0.0).rgb;
  vec3 r = c;
  if (pc.p.z > 0.0) {
    vec3 g = (pc.p.w > 0.5) ? pow(c, vec3(1.0/2.2)) : c;
    g *= pc.gain.rgb;
    float l = dot(g, vec3(0.2126, 0.7152, 0.0722));
    g = mix(vec3(l), g, pc.p.x);
    g = (g - 0.5) * pc.p.y + 0.5;
    g = clamp(g, 0.0, 1.0);
    if (pc.p.w > 0.5) g = pow(g, vec3(2.2));
    r = mix(c, g, pc.p.z);
  }
  int type = int(pc.q.x + 0.5);
  if (type > 0) {
    // the sampled value is linear for an sRGB target and display-encoded for a UNORM one; the matrices want linear light
    vec3 lin = (pc.p.w > 0.5) ? r : pow(clamp(r, 0.0, 1.0), vec3(2.2));
    lin = daltonize(lin, type, pc.q.y);
    r = (pc.p.w > 0.5) ? lin : pow(lin, vec3(1.0/2.2));
  }
  if (pc.q.w > 0.5) {   // Reduce flashing: one gain for the whole frame, chosen so its mean brightness moves no faster than the chosen rate
    const float fg = texture(st, vec2(0.5)).y;
    r = clamp(pc.p.w > 0.5 ? r * fg : r * pow(fg, 1.0 / 2.2), 0.0, 1.0);
  }
  o = vec4(r, 1.0);
}
