//! Ground files next to a course's ground meshes, decoded 2026-11-15 (owner list item 2a):
//!
//! - `<model>.DC4A8177`: collision. FlatBuffers root { shapes: [Shape] }. Shape { s0 pos: Vec3, s1 rot: Vec3 (Euler
//!   radians), s2 scale: Vec3, s3 verts: [Vec3] (struct vector, shape space), s4 faces: [Face], s5 name hash: u32 }.
//!   Face { s0 idx: [u32; 3], s1 flags: u32 (surface kind) }. The big identity-transform shapes are the walkable
//!   ground split into chunks; the small transformed ones are convex rocks and props.
//! - `<model>.navmshb`: FlatBuffers wrapper around a Recast/Detour navmesh tile (magic `DNAV`, version 7) built with
//!   64-bit poly refs (`dtLink` is 16 bytes). Layout after the 100-byte `dtMeshHeader`: verts, polys (32 bytes),
//!   links, detail meshes (12), detail verts, detail tris (4), BV tree nodes (16), off-mesh links.
//!
//! Terrain edits keep every file's structure and only move heights, so offsets, counts and indices never change.

use glam::{EulerRot, Mat4, Quat, Vec3};

fn u16_at(b: &[u8], p: usize) -> Option<u16> {
    Some(u16::from_le_bytes(b.get(p..p + 2)?.try_into().ok()?))
}
fn u32_at(b: &[u8], p: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(p..p + 4)?.try_into().ok()?))
}
fn i32_at(b: &[u8], p: usize) -> Option<i32> {
    Some(i32::from_le_bytes(b.get(p..p + 4)?.try_into().ok()?))
}
pub fn f32_at(b: &[u8], p: usize) -> Option<f32> {
    Some(f32::from_le_bytes(b.get(p..p + 4)?.try_into().ok()?))
}
pub fn put_f32(b: &mut [u8], p: usize, v: f32) {
    b[p..p + 4].copy_from_slice(&v.to_le_bytes());
}
fn put_u16(b: &mut [u8], p: usize, v: u16) {
    b[p..p + 2].copy_from_slice(&v.to_le_bytes());
}
pub fn vec3_at(b: &[u8], p: usize) -> Option<Vec3> {
    Some(Vec3::new(f32_at(b, p)?, f32_at(b, p + 4)?, f32_at(b, p + 8)?))
}

/// Absolute position of field `slot` of the table at `t`.
fn field(b: &[u8], t: usize, slot: usize) -> Option<usize> {
    let vt = (t as i64 - i32_at(b, t)? as i64) as usize;
    let vsize = u16_at(b, vt)? as usize;
    let e = 4 + 2 * slot;
    if e + 2 > vsize {
        return None;
    }
    let o = u16_at(b, vt + e)? as usize;
    (o != 0).then_some(t + o)
}
fn indirect(b: &[u8], p: usize) -> Option<usize> {
    Some(p + u32_at(b, p)? as usize)
}

#[derive(Clone, Debug)]
pub struct Shape {
    pub pos: Vec3,
    pub rot: Vec3,
    pub scale: Vec3,
    /// File offset of the first vertex (12 bytes each).
    pub verts_off: usize,
    pub verts: Vec<Vec3>,
    pub tris: Vec<[u32; 3]>,
    pub flags: Vec<u32>,
    pub hash: u32,
}

impl Shape {
    pub fn xf(&self) -> Mat4 {
        let q = Quat::from_euler(EulerRot::XYZ, self.rot.x, self.rot.y, self.rot.z);
        Mat4::from_scale_rotation_translation(self.scale, q, self.pos)
    }
    pub fn identity(&self) -> bool {
        self.pos.length() < 1e-4 && self.rot.length() < 1e-4 && (self.scale - Vec3::ONE).length() < 1e-4
    }
}

pub fn parse_collision(b: &[u8]) -> Result<Vec<Shape>, String> {
    let root = u32_at(b, 0).ok_or("short file")? as usize;
    let v = indirect(b, field(b, root, 0).ok_or("no shape list")?).ok_or("bad shape list")?;
    let n = u32_at(b, v).ok_or("bad shape count")? as usize;
    let mut out = Vec::with_capacity(n);
    for i in 0..n {
        let e = v + 4 + 4 * i;
        let t = indirect(b, e).ok_or("bad shape offset")?;
        let f = |s: usize| field(b, t, s);
        let v3 = |s: usize, d: Vec3| f(s).and_then(|p| vec3_at(b, p)).unwrap_or(d);
        let (verts_off, verts) = match f(3).and_then(|p| indirect(b, p)) {
            Some(p) => {
                let n = u32_at(b, p).ok_or("bad vertex count")? as usize;
                let vs: Option<Vec<Vec3>> = (0..n).map(|k| vec3_at(b, p + 4 + 12 * k)).collect();
                (p + 4, vs.ok_or("vertex list runs past the file")?)
            }
            None => (0, vec![]),
        };
        let mut tris = vec![];
        let mut flags = vec![];
        if let Some(p) = f(4).and_then(|p| indirect(b, p)) {
            let nf = u32_at(b, p).ok_or("bad face count")? as usize;
            for k in 0..nf {
                let ft = indirect(b, p + 4 + 4 * k).ok_or("bad face offset")?;
                let ip = field(b, ft, 0).and_then(|q| indirect(b, q)).ok_or("face without indices")?;
                if u32_at(b, ip) != Some(3) {
                    return Err(format!("shape {i} face {k}: not a triangle"));
                }
                let idx = [u32_at(b, ip + 4), u32_at(b, ip + 8), u32_at(b, ip + 12)];
                let [Some(a), Some(c), Some(d)] = idx else { return Err("face indices run past the file".into()) };
                if a as usize >= verts.len() || c as usize >= verts.len() || d as usize >= verts.len() {
                    return Err(format!("shape {i} face {k}: index out of range"));
                }
                tris.push([a, c, d]);
                flags.push(field(b, ft, 1).and_then(|q| u32_at(b, q)).unwrap_or(0));
            }
        }
        out.push(Shape {
            pos: v3(0, Vec3::ZERO),
            rot: v3(1, Vec3::ZERO),
            scale: v3(2, Vec3::ONE),
            verts_off,
            verts,
            tris,
            flags,
            hash: f(5).and_then(|p| u32_at(b, p)).unwrap_or(0),
        });
    }
    Ok(out)
}

/// Surface flags (a face's s1, two words) of the first prop shape (one with a transform), else of the first face.
pub fn prop_face_flags(b: &[u8]) -> Option<[u32; 2]> {
    let root = u32_at(b, 0)? as usize;
    let v = indirect(b, field(b, root, 0)?)?;
    let mut first = None;
    for i in 0..u32_at(b, v)? as usize {
        let t = indirect(b, v + 4 + 4 * i)?;
        let Some(faces) = field(b, t, 4).and_then(|p| indirect(b, p)) else { continue };
        if u32_at(b, faces)? == 0 {
            continue;
        }
        let ft = indirect(b, faces + 4)?;
        let Some(p) = field(b, ft, 1) else { continue };
        let fl = [u32_at(b, p)?, u32_at(b, p + 4)?];
        let is = |s: usize, d: f32| field(b, t, s).and_then(|p| vec3_at(b, p)).is_none_or(|x| (x - Vec3::splat(d)).length() < 1e-4);
        if !(is(0, 0.0) && is(1, 0.0) && is(2, 1.0)) {
            return Some(fl);
        }
        first.get_or_insert(fl);
    }
    first
}

/// Appends one collision shape (identity transform, vertices in the ground model's space) to a DC4A8177 file. The new
/// shape list, the shape table, its vertices and faces go in a block right after the root table; everything after it
/// shifts by the block size, so only the root's list field crosses the insertion point and is re-pointed. The old list
/// stays behind as unused bytes. The result is checked by parsing it back.
pub fn append_shape(b: &[u8], verts: &[Vec3], tris: &[[u32; 3]], flags: [u32; 2], hash: u32) -> Result<Vec<u8>, String> {
    let root = u32_at(b, 0).ok_or("short file")? as usize;
    let vt = (root as i64 - i32_at(b, root).ok_or("bad root")? as i64) as usize;
    let tsize = u16_at(b, vt + 2).ok_or("bad vtable")? as usize;
    let fld = field(b, root, 0).ok_or("no shape list")?;
    let old = indirect(b, fld).ok_or("bad shape list")?;
    let p_ins = root + tsize;
    if vt >= root || p_ins % 4 != 0 || old < p_ins {
        return Err("unexpected collision file layout".into());
    }
    if verts.is_empty() || tris.iter().flatten().any(|&i| i as usize >= verts.len()) {
        return Err("collision shape: no vertices or an index out of range".into());
    }
    let before = parse_collision(b)?;
    let n = before.len();
    // block bytes; positions are absolute (block starts at p_ins)
    let mut k: Vec<u8> = vec![];
    let pad = |k: &mut Vec<u8>, a: usize| {
        while (p_ins + k.len()) % a != 0 {
            k.push(0);
        }
    };
    let put = |k: &mut Vec<u8>, at: usize, v: u32| k[at - p_ins..at - p_ins + 4].copy_from_slice(&v.to_le_bytes());
    let u32s = |k: &mut Vec<u8>, vs: &[u32]| vs.iter().for_each(|v| k.extend_from_slice(&v.to_le_bytes()));
    // shape list
    let vp = p_ins;
    u32s(&mut k, &[(n + 1) as u32]);
    k.resize(k.len() + 4 * (n + 1), 0);
    // vtables: shape {pos, rot, scale, verts, faces, hash} in a 52-byte table; face {indices, flags} in 16 bytes
    let svt = p_ins + k.len();
    [16u16, 52, 4, 16, 28, 40, 44, 48].iter().for_each(|v| k.extend_from_slice(&v.to_le_bytes()));
    let fvt = p_ins + k.len();
    [8u16, 16, 4, 8].iter().for_each(|v| k.extend_from_slice(&v.to_le_bytes()));
    pad(&mut k, 4);
    let tp = p_ins + k.len();
    u32s(&mut k, &[(tp - svt) as u32]);
    [0f32, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0].iter().for_each(|v| k.extend_from_slice(&v.to_le_bytes()));
    u32s(&mut k, &[0, 0, hash]);
    // vertices
    let vvp = p_ins + k.len();
    u32s(&mut k, &[verts.len() as u32]);
    verts.iter().for_each(|v| v.to_array().iter().for_each(|x| k.extend_from_slice(&x.to_le_bytes())));
    put(&mut k, tp + 40, (vvp - (tp + 40)) as u32);
    // faces
    let fvp = p_ins + k.len();
    u32s(&mut k, &[tris.len() as u32]);
    k.resize(k.len() + 4 * tris.len(), 0);
    put(&mut k, tp + 44, (fvp - (tp + 44)) as u32);
    for (i, t) in tris.iter().enumerate() {
        let ft = p_ins + k.len();
        u32s(&mut k, &[(ft - fvt) as u32, 0, flags[0], flags[1]]);
        let ip = p_ins + k.len();
        u32s(&mut k, &[3, t[0], t[1], t[2]]);
        put(&mut k, ft + 4, (ip - (ft + 4)) as u32);
        let at = fvp + 4 + 4 * i;
        put(&mut k, at, (ft - at) as u32);
    }
    pad(&mut k, 16);
    let size = k.len();
    for i in 0..n {
        let at = vp + 4 + 4 * i;
        let target = indirect(b, old + 4 + 4 * i).ok_or("bad shape offset")? + size;
        put(&mut k, at, (target - at) as u32);
    }
    put(&mut k, vp + 4 + 4 * n, (tp - (vp + 4 + 4 * n)) as u32);
    let mut out = Vec::with_capacity(b.len() + size);
    out.extend_from_slice(&b[..p_ins]);
    out.extend_from_slice(&k);
    out.extend_from_slice(&b[p_ins..]);
    out[fld..fld + 4].copy_from_slice(&((vp - fld) as u32).to_le_bytes());
    // check: the old shapes unchanged, the new one as given
    let after = parse_collision(&out)?;
    let same = |a: &Shape, c: &Shape| a.pos == c.pos && a.rot == c.rot && a.scale == c.scale && a.verts == c.verts && a.tris == c.tris && a.flags == c.flags && a.hash == c.hash;
    let new = after.last().ok_or("no shapes after append")?;
    if after.len() != n + 1 || !before.iter().zip(&after).all(|(a, c)| same(a, c)) || new.verts != verts || new.tris != tris || new.hash != hash || new.flags.iter().any(|&f| f != flags[0]) {
        return Err("collision append: read-back differs".into());
    }
    Ok(out)
}

pub const DNAV: u32 = u32::from_le_bytes(*b"VAND");

/// One Detour tile inside a navmshb file. Offsets are absolute file positions.
#[derive(Clone, Debug)]
pub struct NavTile {
    pub at: usize,
    pub poly_count: usize,
    pub vert_count: usize,
    pub max_links: usize,
    pub detail_meshes: usize,
    pub detail_verts: usize,
    pub detail_tris: usize,
    pub bv_nodes: usize,
    pub off_mesh: usize,
    pub bmin: Vec3,
    pub bmax: Vec3,
    pub quant: f32,
    pub walkable: (f32, f32, f32),
    pub verts_at: usize,
    pub polys_at: usize,
    pub dmesh_at: usize,
    pub dverts_at: usize,
    pub dtris_at: usize,
    pub bv_at: usize,
    pub end: usize,
}

const HEADER: usize = 100;
const POLY: usize = 32;
const LINK: usize = 16;
const BMIN: usize = 72;
const BMAX: usize = 84;

fn parse_tile(b: &[u8], at: usize) -> Option<NavTile> {
    if u32_at(b, at)? != DNAV || u32_at(b, at + 4)? != 7 {
        return None;
    }
    let c = |k: usize| u32_at(b, at + 24 + 4 * k).map(|v| v as usize);
    let (poly_count, vert_count, max_links) = (c(0)?, c(1)?, c(2)?);
    let (detail_meshes, detail_verts, detail_tris) = (c(3)?, c(4)?, c(5)?);
    let (bv_nodes, off_mesh) = (c(6)?, c(7)?);
    // header: magic, version, x, y, layer, userId, 9 counts (24..60), walkable height/radius/climb, bmin, bmax, quant
    let walkable = (f32_at(b, at + 60)?, f32_at(b, at + 64)?, f32_at(b, at + 68)?);
    let bmin = vec3_at(b, at + BMIN)?;
    let bmax = vec3_at(b, at + BMAX)?;
    let quant = f32_at(b, at + 96)?;
    let verts_at = at + HEADER;
    let polys_at = verts_at + 12 * vert_count;
    let links_at = polys_at + POLY * poly_count;
    let dmesh_at = links_at + LINK * max_links;
    let dverts_at = dmesh_at + 12 * detail_meshes;
    let dtris_at = dverts_at + 12 * detail_verts;
    let bv_at = dtris_at + 4 * detail_tris;
    let end = bv_at + 16 * bv_nodes + 36 * off_mesh;
    (end <= b.len() && detail_meshes == poly_count).then_some(NavTile {
        at,
        poly_count,
        vert_count,
        max_links,
        detail_meshes,
        detail_verts,
        detail_tris,
        bv_nodes,
        off_mesh,
        bmin,
        bmax,
        quant,
        walkable,
        verts_at,
        polys_at,
        dmesh_at,
        dverts_at,
        dtris_at,
        bv_at,
        end,
    })
}

/// Every Detour tile in the file (scans 4-aligned positions for the magic and checks the layout fits).
pub fn find_tiles(b: &[u8]) -> Vec<NavTile> {
    let mut out = vec![];
    let mut p = 0;
    while p + HEADER <= b.len() {
        if let Some(t) = parse_tile(b, p) {
            p = t.end;
            out.push(t);
        } else {
            p += 4;
        }
    }
    out
}

impl NavTile {
    pub fn vert(&self, b: &[u8], i: usize) -> Vec3 {
        vec3_at(b, self.verts_at + 12 * i).unwrap_or_default()
    }
    pub fn dvert(&self, b: &[u8], i: usize) -> Vec3 {
        vec3_at(b, self.dverts_at + 12 * i).unwrap_or_default()
    }
    /// Polygon `p`: (vertex indices, detail vert base, detail vert count).
    pub fn poly(&self, b: &[u8], p: usize) -> (Vec<usize>, usize, usize) {
        let at = self.polys_at + POLY * p;
        // dtPoly: firstLink u32, verts[6] u16, neis[6] u16, flags u16 (+28), vertCount u8 (+30), areaAndType u8 (+31)
        let n = b[at + 30] as usize;
        let vs = (0..n.min(6)).map(|k| u16_at(b, at + 4 + 2 * k).unwrap_or(0) as usize).collect();
        let d = self.dmesh_at + 12 * p;
        (vs, u32_at(b, d).unwrap_or(0) as usize, b[d + 8] as usize)
    }
    /// Off-mesh polygons (type bit set) have no detail mesh and are left alone.
    fn is_ground_poly(&self, b: &[u8], p: usize) -> bool {
        b[self.polys_at + POLY * p + 31] >> 6 == 0
    }

    /// Adds `dy(point)` to every vertex height, then refits the vertical bounds of the polygons that moved.
    pub fn displace(&self, b: &mut [u8], dy: &dyn Fn(Vec3) -> f32) {
        let mut moved_v = vec![false; self.vert_count];
        let mut moved_d = vec![false; self.detail_verts];
        for i in 0..self.vert_count {
            let v = self.vert(b, i);
            let d = dy(v);
            if d != 0.0 {
                put_f32(b, self.verts_at + 12 * i + 4, v.y + d);
                moved_v[i] = true;
            }
        }
        for i in 0..self.detail_verts {
            let v = self.dvert(b, i);
            let d = dy(v);
            if d != 0.0 {
                put_f32(b, self.dverts_at + 12 * i + 4, v.y + d);
                moved_d[i] = true;
            }
        }
        let moved: Vec<bool> = (0..self.poly_count)
            .map(|p| {
                let (vs, db, dn) = self.poly(b, p);
                vs.iter().any(|&i| moved_v.get(i) == Some(&true)) || (0..dn).any(|k| moved_d.get(db + k) == Some(&true))
            })
            .collect();
        self.refit(b, &moved);
    }

    /// Refits the vertical bounds of the moved polygons: their BV leaves, the BV nodes above them and the header.
    /// Unmoved leaves keep their original bytes: the original heights come from Recast data that is not in the file,
    /// so they cannot be recomputed exactly. Moved leaves get conservative bounds (floor low, ceil high), which
    /// Detour's queries accept. When the ground drops below the tile's base, the base moves down by whole
    /// quantization steps and every untouched node shifts by the same count.
    pub fn refit(&self, b: &mut [u8], moved: &[bool]) {
        if !moved.iter().any(|&m| m) {
            return;
        }
        let mut lo = f32::MAX;
        let mut hi = f32::MIN;
        let mut poly_y = vec![(f32::MAX, f32::MIN); self.poly_count];
        for p in 0..self.poly_count {
            if !moved.get(p).copied().unwrap_or(false) || !self.is_ground_poly(b, p) {
                continue;
            }
            let (vs, db, dn) = self.poly(b, p);
            let ys = vs.iter().map(|&i| self.vert(b, i).y).chain((0..dn).map(|k| self.dvert(b, db + k).y));
            for y in ys {
                poly_y[p].0 = poly_y[p].0.min(y);
                poly_y[p].1 = poly_y[p].1.max(y);
            }
            lo = lo.min(poly_y[p].0);
            hi = hi.max(poly_y[p].1);
        }
        if lo > hi {
            return;
        }
        let (mut base, mut shift) = (self.bmin.y, 0u16);
        if lo < base {
            let k = ((base - lo) * self.quant).ceil();
            base -= k / self.quant;
            shift = k as u16;
        }
        put_f32(b, self.at + BMIN + 4, base);
        put_f32(b, self.at + BMAX + 4, self.bmax.y.max(hi));
        let q_lo = |y: f32| ((y - base) * self.quant).floor().clamp(0.0, 65535.0) as u16;
        let q_hi = |y: f32| ((y - base) * self.quant).ceil().clamp(0.0, 65535.0) as u16;
        let node = |k: usize| self.bv_at + 16 * k;
        let mut touched = vec![false; self.bv_nodes];
        for k in 0..self.bv_nodes {
            let i = i32_at(b, node(k) + 12).unwrap_or(0);
            if i >= 0 && (i as usize) < self.poly_count && poly_y[i as usize].0 <= poly_y[i as usize].1 {
                let (y0, y1) = poly_y[i as usize];
                put_u16(b, node(k) + 2, q_lo(y0));
                put_u16(b, node(k) + 8, q_hi(y1));
                touched[k] = true;
            } else if shift > 0 {
                for off in [2, 8] {
                    let v = u16_at(b, node(k) + off).unwrap_or(0);
                    put_u16(b, node(k) + off, v.saturating_add(shift));
                }
            }
        }
        // internal nodes over a moved leaf take the union of the nodes they span (children come after the parent)
        for k in (0..self.bv_nodes).rev() {
            let i = i32_at(b, node(k) + 12).unwrap_or(0);
            if i < 0 {
                let end = (k + (-i) as usize).min(self.bv_nodes);
                if !(k + 1..end).any(|j| touched[j]) {
                    continue;
                }
                touched[k] = true;
                let (mut y0, mut y1) = (u16::MAX, 0u16);
                for j in k + 1..end {
                    y0 = y0.min(u16_at(b, node(j) + 2).unwrap_or(0));
                    y1 = y1.max(u16_at(b, node(j) + 8).unwrap_or(0));
                }
                if y0 <= y1 {
                    put_u16(b, node(k) + 2, y0);
                    put_u16(b, node(k) + 8, y1);
                }
            }
        }
    }

    /// Height of the detail surface at (x, z) closest to `near_y`, when the point is inside a polygon of this tile.
    pub fn height_at(&self, b: &[u8], x: f32, z: f32, near_y: f32) -> Option<f32> {
        let mut best: Option<f32> = None;
        for p in 0..self.poly_count {
            if !self.is_ground_poly(b, p) {
                continue;
            }
            let (vs, db, _) = self.poly(b, p);
            let d = self.dmesh_at + 12 * p;
            let tb = u32_at(b, d + 4)? as usize;
            let tn = b[d + 9] as usize;
            let pt = |k: u8| {
                let k = k as usize;
                if k < vs.len() {
                    self.vert(b, vs[k])
                } else {
                    self.dvert(b, db + k - vs.len())
                }
            };
            for t in 0..tn {
                let at = self.dtris_at + 4 * (tb + t);
                let (a, c, e) = (pt(b[at]), pt(b[at + 1]), pt(b[at + 2]));
                if let Some(y) = tri_height(a, c, e, x, z) {
                    if best.map_or(true, |bb| (y - near_y).abs() < (bb - near_y).abs()) {
                        best = Some(y);
                    }
                }
            }
        }
        best
    }
}

/// Height of triangle abc above (x, z) when (x, z) is inside its xz projection.
pub fn tri_height(a: Vec3, b: Vec3, c: Vec3, x: f32, z: f32) -> Option<f32> {
    let v0 = (c.x - a.x, c.z - a.z);
    let v1 = (b.x - a.x, b.z - a.z);
    let v2 = (x - a.x, z - a.z);
    let den = v0.0 * v1.1 - v1.0 * v0.1;
    if den.abs() < 1e-9 {
        return None;
    }
    let u = (v2.0 * v1.1 - v1.0 * v2.1) / den;
    let v = (v0.0 * v2.1 - v2.0 * v0.1) / den;
    let eps = 1e-4;
    (u >= -eps && v >= -eps && u + v <= 1.0 + eps).then(|| a.y + u * (c.y - a.y) + v * (b.y - a.y))
}

/// Moves collision vertices of every shape by `dy` at their world position, scaled by `weight` of that position.
/// `inst` is the model instance transform.
pub fn displace_collision(b: &mut [u8], shapes: &[Shape], inst: Mat4, dy: &dyn Fn(f32, f32) -> f32, weight: &dyn Fn(Vec3) -> f32) {
    for s in shapes {
        let to_world = inst * s.xf();
        let to_local = to_world.inverse();
        for (k, v) in s.verts.iter().enumerate() {
            let w = to_world.transform_point3(*v);
            let d = dy(w.x, w.z) * weight(w);
            if d == 0.0 {
                continue;
            }
            let l = to_local.transform_point3(w + Vec3::Y * d);
            let at = s.verts_off + 12 * k;
            put_f32(b, at, l.x);
            put_f32(b, at + 4, l.y);
            put_f32(b, at + 8, l.z);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Smallest collision file: root at 12 (vtable at 4) whose list field points at an empty vector at 20.
    fn empty_file() -> Vec<u8> {
        let mut b = vec![];
        b.extend_from_slice(&12u32.to_le_bytes());
        [6u16, 8, 4, 0].iter().for_each(|v| b.extend_from_slice(&v.to_le_bytes()));
        b.extend_from_slice(&8i32.to_le_bytes());
        b.extend_from_slice(&4u32.to_le_bytes());
        b.extend_from_slice(&0u32.to_le_bytes());
        b
    }

    #[test]
    fn append_shape_twice_reads_back() {
        let tri = [Vec3::ZERO, Vec3::X, Vec3::Z];
        let one = append_shape(&empty_file(), &tri, &[[0, 1, 2]], [5, 7], 0xabc).unwrap();
        let quad = [Vec3::ZERO, Vec3::X, Vec3::new(1.0, 0.0, 1.0), Vec3::Z];
        let two = append_shape(&one, &quad, &[[0, 1, 2], [0, 2, 3]], [5, 7], 0xdef).unwrap();
        let s = parse_collision(&two).unwrap();
        assert_eq!(s.len(), 2);
        assert_eq!((s[0].verts.as_slice(), s[0].hash), (&tri[..], 0xabc));
        assert_eq!((s[1].tris.as_slice(), s[1].flags.as_slice(), s[1].hash), (&[[0, 1, 2], [0, 2, 3]][..], &[5u32, 5][..], 0xdef));
        assert_eq!(prop_face_flags(&two), Some([5, 7]));
        assert!(append_shape(&empty_file(), &tri, &[[0, 1, 3]], [0, 0], 1).is_err(), "index out of range");
    }
}
