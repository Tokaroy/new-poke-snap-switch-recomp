#version 450
// Reduce flashing, step 1 (host/mods/look.cpp): from the mean brightness of this frame (the 1x1 mip) and last frame's output mean, pick the gain that
// keeps the output mean within one step of last frame's. Written to a 1x1 float image: x = output mean, y = gain, z = input mean (all in sqrt-of-linear
// units, so a step is about equally visible in dark and bright scenes).
layout(set=0,binding=0) uniform sampler2D tex;
layout(set=0,binding=1) uniform sampler2D st;   // last frame's state
layout(push_constant) uniform P {
  vec4 a;   // max step this frame, reset (1 = no history yet), min gain, max gain
  vec4 p;   // 0, 0, 0, srgb flag
  vec4 q;
} pc;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 o;
void main(){
  vec3 m = textureLod(tex, vec2(0.5), 30.0).rgb;
  float lin = dot(pc.p.w > 0.5 ? m : pow(m, vec3(2.2)), vec3(0.2126, 0.7152, 0.0722));
  float inM = sqrt(max(lin, 1e-5));
  float prev = pc.a.y > 0.5 ? inM : texture(st, vec2(0.5)).x;
  float tgt = clamp(inM, prev - pc.a.x, prev + pc.a.x);
  float gain = clamp(tgt / inM, pc.a.z, pc.a.w);
  float outM = min(inM * gain, 1.0);
  o = vec4(outM, gain, inM, 1.0);
}
