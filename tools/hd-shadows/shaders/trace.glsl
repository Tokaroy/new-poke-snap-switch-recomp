// Shared by bake.comp and points.comp: the course scene and the sun visibility test.
//
// The scene acceleration structure holds two geometries: solid triangles (opaque, never reported as candidates) and
// alpha-tested cards (leaves, grass, fences). A candidate hit on a card is confirmed only where the card's cut-out mask
// (its base colour alpha, decoded by scripts/hd/hdshadows_alpha.py) is above the material's alpha reference, so light
// passes between the leaves as it does in the game's own bake.

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

struct CardTri {
    vec4 uv01;   // u0 v0 u1 v1 (TextureCoordinate0)
    vec4 uv2m;   // u2 v2, mask index (as float bits), pad
};
layout(set = 0, binding = 4, scalar) readonly buffer CardTris { CardTri cards[]; };
struct Mask {
    uint offset; // first byte in maskBytes
    uint w;
    uint h;
    uint ref;    // alpha reference 0..255; a texel at or below it lets light through
};
layout(set = 0, binding = 5, scalar) readonly buffer Masks { Mask masks[]; };
layout(set = 0, binding = 6, scalar) readonly buffer MaskData { uint maskWords[]; };

layout(push_constant, scalar) uniform PC {
    vec3 sunDir;       // unit vector from the surface towards the sun
    float cone;        // tangent of the sun's angular radius
    uint size;         // bake: atlas width = height in texels; points: point count
    uint samples;
    float bias;        // normal offset in world units
    float tmin;
    uint rowStart;     // bake: first row of this dispatch
    uint seed;
    uint flags;        // FLAG_*
} pc;

const uint FLAG_NO_NDL = 1u;      // a receiver facing away from the sun is not shadowed by that alone
const uint FLAG_CULL_BACK = 2u;   // shadow rays ignore triangles seen from behind
const uint FLAG_CULL_FRONT = 4u;

uint hash(uint x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

uint maskByte(uint i) {
    return (maskWords[i >> 2] >> ((i & 3u) * 8u)) & 0xFFu;
}

bool cardBlocks(uint prim, vec2 bc) {
    CardTri c = cards[prim];
    vec2 uv = c.uv01.xy * (1.0 - bc.x - bc.y) + c.uv01.zw * bc.x + c.uv2m.xy * bc.y;
    Mask m = masks[floatBitsToUint(c.uv2m.z)];
    uv = fract(uv);
    uint x = min(uint(uv.x * float(m.w)), m.w - 1u);
    uint y = min(uint(uv.y * float(m.h)), m.h - 1u);
    return maskByte(m.offset + y * m.w + x) > m.ref;
}

bool occluded(vec3 o, vec3 dir) {
    rayQueryEXT rq;
    uint rf = gl_RayFlagsTerminateOnFirstHitEXT;
    if ((pc.flags & FLAG_CULL_BACK) != 0u) rf |= gl_RayFlagsCullBackFacingTrianglesEXT;
    if ((pc.flags & FLAG_CULL_FRONT) != 0u) rf |= gl_RayFlagsCullFrontFacingTrianglesEXT;
    rayQueryInitializeEXT(rq, tlas, rf, 0xFF, o, pc.tmin, dir, 1.0e5);
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
            if (cardBlocks(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false), rayQueryGetIntersectionBarycentricsEXT(rq, false)))
                rayQueryConfirmIntersectionEXT(rq);
        }
    }
    return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

// Occluded fraction of the sun's disc seen from `pos` (geometric normal `gn`, shading normal `nrm`).
// `twoSided`: the receiver is a card lit from either side (its back faces the sun on half of the course).
float sunOcclusion(vec3 sunDir, vec3 pos, vec3 gn, vec3 nrm, bool twoSided, uint rnd) {
    float ndl = dot(nrm, sunDir);
    if (twoSided && ndl < 0.0) {
        gn = -gn;
        ndl = -ndl;
    }
    if (ndl <= 0.0 && (pc.flags & FLAG_NO_NDL) == 0u) return 1.0;
    if (dot(gn, sunDir) < 0.0) gn = -gn;
    vec3 o = pos + gn * pc.bias;
    vec3 tu = normalize(abs(sunDir.y) < 0.99 ? cross(sunDir, vec3(0, 1, 0)) : cross(sunDir, vec3(1, 0, 0)));
    vec3 tv = cross(sunDir, tu);
    float rot = float(rnd & 0xFFFFu) * (6.2831853 / 65536.0);
    float hit = 0.0;
    for (uint i = 0; i < pc.samples; i++) {
        // Vogel disk, rotated per texel; one sample is the disc's centre
        float r = pc.samples == 1u ? 0.0 : sqrt((float(i) + 0.5) / float(pc.samples));
        float a = float(i) * 2.39996323 + rot;
        vec3 dir = normalize(sunDir + (tu * cos(a) + tv * sin(a)) * (r * pc.cone));
        if (occluded(o, dir)) hit += 1.0;
    }
    return hit / float(pc.samples);
}
