// SPDX-License-Identifier: MIT
// Shared helpers of the lens effects (nvn_lens_fx.cpp): bokeh depth of field and motion blur. Our own code, no game data.
// Depth of the scene is the game's reversed infinite z (ndc 1 = near, 0 = infinity); the other modes keep the helper usable for finite or forward depth.
float linZ(float d, float near, float far, float mode) {
    if (mode < 0.5) return d > 0.0 ? min(near / d, 1.0e5) : 1.0e5;
    if (mode < 1.5) return near * far / (near + d * (far - near));
    return near * far / (far - d * (far - near));
}
// interleaved gradient noise (Jimenez 2014), one value per pixel
float ign(vec2 p, float frame) {
    p += 5.588238 * frame;
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}
