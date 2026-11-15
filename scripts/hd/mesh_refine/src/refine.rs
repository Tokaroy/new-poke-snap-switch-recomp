// SPDX-License-Identifier: MIT
// Offline mesh refinement for Snap Recomp model packs.
//
// Each refined mesh object keeps the game's own vertex layout (attribute types, buffer split, strides) and its
// original vertices bit-for-bit at indices 0..V-1. New vertices are appended. The object's index range becomes
//
//   H (9 indices) | original triangles (No) | S (9) | refined triangles (Nr) | T (9)
//
// H, S and T are degenerate triangles (draw nothing): H = 0,0,0 1,1,1 0,0,0  S = 1,1,1 0,0,0 1,1,1  T = 0,0,0 0,0,0 1,1,1.
// The runtime (runtime/nvn/nvn_model.cpp) draws only the refined range in the live view and only the original range in
// the photo evaluation pass, so photo scoring sees exactly the original geometry.
//
// Refinement: 1:4 split per level. Edge midpoints are the PN-triangle cubic edge at t = 0.5 built from the artist's
// normals: m = (P0 + P1) / 2 - (w01 N0 + w10 N1) / 8, w_ij = (Pj - Pi) . Ni. Edges stay straight where the two sides
// disagree on the endpoint normals (hard edges, creases), on non-manifold edges, and on edges with a zero normal.
// Everything about a midpoint that decides the surface (position, skin weights) is computed once per position-welded
// edge in a canonical order, so both sides of a UV seam get bit-identical new vertices and the mesh cannot crack, at
// rest or in motion. Attributes (normal, tangent, binormal, UV, color) are interpolated per side, so seams keep their
// own UVs. Normals, tangents and binormals are renormalized (tangent frames re-orthogonalized to the normal).
use ssbh_lib::formats::mesh::*;
use std::collections::HashMap;

pub const HDR: [u32; 9] = [0, 0, 0, 1, 1, 1, 0, 0, 0];
pub const SEP: [u32; 9] = [1, 1, 1, 0, 0, 0, 1, 1, 1];
pub const TRL: [u32; 9] = [0, 0, 0, 0, 0, 0, 1, 1, 1];

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Kind {
    Position,
    Normal,
    Tangent,
    Binormal,
    Other,
}

pub struct Attr {
    pub kind: Kind,
    pub buf: usize,
    pub off: usize,
    pub ty: AttributeDataTypeV10,
}

pub fn comps(t: AttributeDataTypeV10) -> (usize, usize) {
    // (component count, bytes)
    match t {
        AttributeDataTypeV10::Float3 => (3, 12),
        AttributeDataTypeV10::Byte4 => (4, 4),
        AttributeDataTypeV10::Float4 => (4, 16),
        AttributeDataTypeV10::HalfFloat4 => (4, 8),
        AttributeDataTypeV10::Float2 => (2, 8),
        AttributeDataTypeV10::HalfFloat2 => (2, 4),
    }
}

fn decode(t: AttributeDataTypeV10, b: &[u8]) -> [f32; 4] {
    let f = |i: usize| f32::from_le_bytes([b[i * 4], b[i * 4 + 1], b[i * 4 + 2], b[i * 4 + 3]]);
    let h = |i: usize| half::f16::from_le_bytes([b[i * 2], b[i * 2 + 1]]).to_f32();
    match t {
        AttributeDataTypeV10::Float3 => [f(0), f(1), f(2), 0.0],
        AttributeDataTypeV10::Float4 => [f(0), f(1), f(2), f(3)],
        AttributeDataTypeV10::Float2 => [f(0), f(1), 0.0, 0.0],
        AttributeDataTypeV10::HalfFloat4 => [h(0), h(1), h(2), h(3)],
        AttributeDataTypeV10::HalfFloat2 => [h(0), h(1), 0.0, 0.0],
        AttributeDataTypeV10::Byte4 => [b[0] as f32 / 255.0, b[1] as f32 / 255.0, b[2] as f32 / 255.0, b[3] as f32 / 255.0],
    }
}

fn encode(t: AttributeDataTypeV10, v: [f32; 4], out: &mut [u8]) {
    let (n, _) = comps(t);
    match t {
        AttributeDataTypeV10::Float3 | AttributeDataTypeV10::Float4 | AttributeDataTypeV10::Float2 => {
            for i in 0..n {
                out[i * 4..i * 4 + 4].copy_from_slice(&v[i].to_le_bytes());
            }
        }
        AttributeDataTypeV10::HalfFloat4 | AttributeDataTypeV10::HalfFloat2 => {
            for i in 0..n {
                out[i * 2..i * 2 + 2].copy_from_slice(&half::f16::from_f32(v[i]).to_le_bytes());
            }
        }
        AttributeDataTypeV10::Byte4 => {
            for i in 0..4 {
                out[i] = (v[i].clamp(0.0, 1.0) * 255.0).round() as u8;
            }
        }
    }
}

pub struct Obj {
    pub attrs: Vec<Attr>,
    pub vals: Vec<Vec<[f32; 4]>>, // [attr][vertex]
    pub weights: Vec<Vec<(u16, f32)>>, // [vertex] -> (bone slot, weight); empty when not skinned
    pub tris: Vec<[u32; 3]>,
    pub max_infl: usize,
}

pub struct Report {
    pub v0: usize,
    pub t0: usize,
    pub v1: usize,
    pub t1: usize,
    pub curved: usize,
    pub straight: usize,
    pub seam_weight_mismatch: usize,
    pub max_disp_ratio: f32,
}

fn v3(a: [f32; 4]) -> glam::Vec3 {
    glam::vec3(a[0], a[1], a[2])
}
fn bits3(a: [f32; 4]) -> [u32; 3] {
    [a[0].to_bits(), a[1].to_bits(), a[2].to_bits()]
}

impl Obj {
    fn find(&self, k: Kind) -> Option<usize> {
        self.attrs.iter().position(|a| a.kind == k)
    }

    // One 1:4 PN level. Returns (curved edges, straight edges, seam weight mismatches, max displacement / edge length).
    pub fn refine_level(&mut self, clamp: f32) -> (usize, usize, usize, f32) {
        let pi = self.find(Kind::Position).expect("no position attribute");
        let ni = self.find(Kind::Normal);
        let nv = self.vals[pi].len();
        // weld by exact position
        let mut wmap: HashMap<[u32; 3], u32> = HashMap::new();
        let mut rep: Vec<u32> = vec![]; // canonical vertex of each weld group
        let weld: Vec<u32> = (0..nv)
            .map(|i| {
                let k = bits3(self.vals[pi][i]);
                let n = rep.len() as u32;
                let id = *wmap.entry(k).or_insert(n);
                if id == n {
                    rep.push(i as u32);
                }
                id
            })
            .collect();
        let mut mismatch = 0;
        if !self.weights.is_empty() {
            for i in 0..nv {
                let r = rep[weld[i] as usize] as usize;
                if r != i && self.weights[i] != self.weights[r] {
                    mismatch += 1;
                }
            }
        }
        let nrm = |i: usize| -> [u32; 3] { ni.map(|n| bits3(self.vals[n][i])).unwrap_or([0; 3]) };
        // per welded edge (lo, hi): faces and the normal pairs (normal at lo, normal at hi) seen from each face
        struct E {
            faces: u32,
            pair: Option<([u32; 3], [u32; 3])>,
            agree: bool,
            lo_v: u32,
            hi_v: u32,
        }
        let mut edges: HashMap<(u32, u32), E> = HashMap::new();
        for t in &self.tris {
            for k in 0..3 {
                let (a, b) = (t[k] as usize, t[(k + 1) % 3] as usize);
                let (wa, wb) = (weld[a], weld[b]);
                if wa == wb {
                    continue;
                }
                let (lo, hi, lv, hv) = if wa < wb { (wa, wb, a, b) } else { (wb, wa, b, a) };
                let p = (nrm(lv), nrm(hv));
                let e = edges.entry((lo, hi)).or_insert(E { faces: 0, pair: None, agree: true, lo_v: lv as u32, hi_v: hv as u32 });
                e.faces += 1;
                match e.pair {
                    None => e.pair = Some(p),
                    Some(q) => {
                        if q != p {
                            e.agree = false
                        }
                    }
                }
            }
        }
        // midpoint position and weights per welded edge, canonical operand order
        let mut mid: HashMap<(u32, u32), (glam::Vec3, Vec<(u16, f32)>)> = HashMap::new();
        let (mut curved, mut straight, mut maxr) = (0, 0, 0f32);
        for (&(lo, hi), e) in &edges {
            let (a, b) = (e.lo_v as usize, e.hi_v as usize);
            let p0 = v3(self.vals[pi][a]);
            let p1 = v3(self.vals[pi][b]);
            let base = (p0 + p1) * 0.5;
            let mut m = base;
            let ok = e.faces <= 2 && e.agree && ni.is_some();
            if ok {
                let n0 = v3(self.vals[ni.unwrap()][a]);
                let n1 = v3(self.vals[ni.unwrap()][b]);
                if n0.length_squared() > 1e-8 && n1.length_squared() > 1e-8 {
                    let (n0, n1) = (n0.normalize(), n1.normalize());
                    let w01 = (p1 - p0).dot(n0);
                    let w10 = (p0 - p1).dot(n1);
                    let mut d = -(n0 * w01 + n1 * w10) / 8.0;
                    let len = (p1 - p0).length();
                    let lim = clamp * len;
                    if d.length() > lim && lim > 0.0 {
                        d = d.normalize() * lim;
                    }
                    if len > 0.0 {
                        maxr = maxr.max(d.length() / len);
                    }
                    m = base + d;
                    curved += 1;
                } else {
                    straight += 1;
                }
            } else {
                straight += 1;
            }
            let w = if self.weights.is_empty() {
                vec![]
            } else {
                let (ra, rb) = (rep[lo as usize] as usize, rep[hi as usize] as usize);
                mix_weights(&self.weights[ra], &self.weights[rb], self.max_infl)
            };
            mid.insert((lo, hi), (m, w));
        }
        // new vertices per (orig a, orig b) pair (unordered); attributes per side, position/weights per welded edge
        let mut newv: HashMap<(u32, u32), u32> = HashMap::new();
        let mut out_tris = Vec::with_capacity(self.tris.len() * 4);
        let tris = std::mem::take(&mut self.tris);
        for t in &tris {
            let mut mids = [0u32; 3];
            for k in 0..3 {
                let (a, b) = (t[k], t[(k + 1) % 3]);
                let key = (a.min(b), a.max(b));
                if let Some(&id) = newv.get(&key) {
                    mids[k] = id;
                    continue;
                }
                let (wa, wb) = (weld[a as usize], weld[b as usize]);
                let id = self.vals[pi].len() as u32;
                for (ai, at) in self.attrs.iter().enumerate() {
                    let x = self.vals[ai][key.0 as usize];
                    let y = self.vals[ai][key.1 as usize];
                    let mut v = [0.5 * (x[0] + y[0]), 0.5 * (x[1] + y[1]), 0.5 * (x[2] + y[2]), 0.5 * (x[3] + y[3])];
                    if at.kind == Kind::Position {
                        let m = if wa == wb { v3(x) } else { mid[&(wa.min(wb), wa.max(wb))].0 };
                        v = [m.x, m.y, m.z, x[3]];
                    }
                    if matches!(at.kind, Kind::Tangent | Kind::Binormal) {
                        v[3] = x[3]; // handedness
                    }
                    self.vals[ai].push(v);
                }
                if !self.weights.is_empty() {
                    let w = if wa == wb { self.weights[key.0 as usize].clone() } else { mid[&(wa.min(wb), wa.max(wb))].1.clone() };
                    self.weights.push(w);
                }
                newv.insert(key, id);
                mids[k] = id;
            }
            let [a, b, c] = *t;
            let [ab, bc, ca] = mids;
            out_tris.push([a, ab, ca]);
            out_tris.push([ab, b, bc]);
            out_tris.push([ca, bc, c]);
            out_tris.push([ab, bc, ca]);
        }
        self.tris = out_tris;
        self.renorm(nv);
        (curved, straight, mismatch, maxr)
    }

    // Normalize normals and re-orthogonalize tangent frames of vertices from `from` on.
    fn renorm(&mut self, from: usize) {
        let ni = self.find(Kind::Normal);
        for i in from..self.vals[0].len() {
            let n = ni.map(|k| v3(self.vals[k][i]).normalize_or_zero());
            if let (Some(k), Some(n)) = (ni, n) {
                let w = self.vals[k][i][3];
                self.vals[k][i] = [n.x, n.y, n.z, w];
            }
            for ai in 0..self.attrs.len() {
                if !matches!(self.attrs[ai].kind, Kind::Tangent | Kind::Binormal) {
                    continue;
                }
                let t = v3(self.vals[ai][i]);
                let t = match n {
                    Some(n) if n.length_squared() > 0.0 => t - n * n.dot(t),
                    _ => t,
                };
                let t = t.normalize_or_zero();
                let w = self.vals[ai][i][3];
                self.vals[ai][i] = [t.x, t.y, t.z, w];
            }
        }
    }
}

pub fn mix_weights(a: &[(u16, f32)], b: &[(u16, f32)], max_infl: usize) -> Vec<(u16, f32)> {
    let mut m: Vec<(u16, f32)> = vec![];
    for &(j, w) in a.iter().chain(b.iter()) {
        match m.iter_mut().find(|x| x.0 == j) {
            Some(x) => x.1 += 0.5 * w,
            None => m.push((j, 0.5 * w)),
        }
    }
    m.sort_by(|x, y| y.1.partial_cmp(&x.1).unwrap().then(x.0.cmp(&y.0)));
    m.truncate(max_infl.max(1));
    let s: f32 = m.iter().map(|x| x.1).sum();
    if s > 0.0 {
        for x in m.iter_mut() {
            x.1 /= s;
        }
    }
    m.sort_by_key(|x| x.0);
    m
}

pub fn kind_of(a: &AttributeV10) -> Kind {
    match a.usage {
        AttributeUsageV9::Position => Kind::Position,
        AttributeUsageV9::Normal => Kind::Normal,
        AttributeUsageV9::Tangent => Kind::Tangent,
        AttributeUsageV9::Binormal => Kind::Binormal,
        _ => Kind::Other,
    }
}

fn stride_of(o: &MeshObject<AttributeV10>, b: usize) -> usize {
    [o.stride0, o.stride1, o.stride2, o.stride3][b] as usize
}
fn vboff_of(o: &MeshObject<AttributeV10>, b: usize) -> usize {
    [o.vertex_buffer0_offset, o.vertex_buffer1_offset, o.vertex_buffer2_offset, o.vertex_buffer3_offset][b] as usize
}

pub fn read_indices(m: &MeshInner<AttributeV10, ssbh_lib::SsbhByteBuffer>, o: &MeshObject<AttributeV10>) -> Vec<u32> {
    let ib = &m.index_buffer.elements;
    let off = o.index_buffer_offset as usize;
    (0..o.vertex_index_count as usize)
        .map(|i| match o.draw_element_type {
            DrawElementType::UnsignedShort => u16::from_le_bytes([ib[off + i * 2], ib[off + i * 2 + 1]]) as u32,
            DrawElementType::UnsignedInt => u32::from_le_bytes(ib[off + i * 4..off + i * 4 + 4].try_into().unwrap()),
        })
        .collect()
}

// Rigging group of one object: bone names in file order and per-vertex (bone slot, weight).
pub fn read_weights(rig: &RiggingGroup<ssbh_lib::SsbhByteBuffer>, nv: usize) -> Vec<Vec<(u16, f32)>> {
    let mut w = vec![vec![]; nv];
    for (slot, bb) in rig.buffers.elements.iter().enumerate() {
        for c in bb.data.elements.chunks_exact(6) {
            let vi = u16::from_le_bytes([c[0], c[1]]) as usize;
            let wt = f32::from_le_bytes([c[2], c[3], c[4], c[5]]);
            if vi < nv {
                w[vi].push((slot as u16, wt));
            }
        }
    }
    w
}

/// Checks the layout invariants this writer relies on (offsets are plain running sums; buffer 2 is 32 bytes per vertex).
pub fn check_layout(m: &MeshInner<AttributeV10, ssbh_lib::SsbhByteBuffer>) -> Result<(), String> {
    let mut vb = [0usize; 4];
    let mut ib = 0usize;
    for o in &m.objects.elements {
        if o.index_buffer_offset as usize == (ib + 3) & !3 {
            ib = (ib + 3) & !3; // most files start each index range 4-byte aligned, some do not: both occur
        }
        let got = [vboff_of(o, 0), vboff_of(o, 1), vboff_of(o, 2), vboff_of(o, 3)];
        if got[0] != vb[0] || got[1] != vb[1] || got[2] != vb[2] || got[3] != vb[3] || o.index_buffer_offset as usize != ib {
            return Err(format!("{:?}#{} offsets {:?}/{} expected {:?}/{}", o.name.to_str(), o.subindex, got, o.index_buffer_offset, vb, ib));
        }
        let n = o.vertex_count as usize;
        vb[0] += stride_of(o, 0) * n;
        vb[1] += stride_of(o, 1) * n;
        vb[2] += 32 * n;
        vb[3] += stride_of(o, 3) * n;
        ib += o.vertex_index_count as usize * if o.draw_element_type == DrawElementType::UnsignedShort { 2 } else { 4 };
    }
    if vb[0] != m.vertex_buffers.elements[0].elements.len() || vb[1] != m.vertex_buffers.elements.get(1).map(|b| b.elements.len()).unwrap_or(0) {
        return Err(format!("vertex buffer sizes {:?} vs {:?}", &vb[..2], m.vertex_buffers.elements.iter().map(|b| b.elements.len()).collect::<Vec<_>>()));
    }
    if ib != m.index_buffer.elements.len() && (ib + 3) & !3 != m.index_buffer.elements.len() {
        return Err(format!("index buffer size {} vs {}", ib, m.index_buffer.elements.len()));
    }
    Ok(())
}

pub struct Options {
    pub levels: usize,
    pub clamp: f32,
    pub only: Vec<String>, // object name#sub filters ("" = all)
    pub min_tris: usize,
}

/// Refines the selected objects of a 1.10 mesh in place. Returns one report line per object.
pub fn refine_mesh(mesh: &mut Mesh, opt: &Options) -> Result<Vec<String>, String> {
    let m = match mesh {
        Mesh::V10(m) => m,
        _ => return Err("only mesh version 1.10 is supported".into()),
    };
    check_layout(m)?;
    let nobj = m.objects.elements.len();
    let mut new_vb: Vec<Vec<u8>> = vec![vec![]; 4];
    let mut new_ib: Vec<u8> = vec![];
    let mut lines = vec![];
    for oi in 0..nobj {
        let o = m.objects.elements[oi].clone();
        let label = format!("{}#{}", o.name.to_str().unwrap_or(""), o.subindex);
        let nv = o.vertex_count as usize;
        let idx = read_indices(m, &o);
        let rig_i = m.rigging_buffers.elements.iter().position(|r| r.mesh_object_name.to_str() == o.name.to_str() && r.mesh_object_subindex == o.subindex);
        let want = (opt.only.is_empty() || opt.only.iter().any(|f| f == &label || f == o.name.to_str().unwrap_or(""))) && idx.len() / 3 >= opt.min_tris;
        let degenerate_in = idx.chunks(3).any(|t| t[0] == t[1] || t[1] == t[2] || t[0] == t[2]);
        let mut obj = None;
        let mut why = String::new();
        if !want {
            why = "not selected".into();
        } else if degenerate_in {
            why = "has degenerate triangles (marker scan would be ambiguous)".into();
        } else if o.vertex_buffer3_offset != 0 || o.stride3 != 0 || o.stride2 != 0 {
            why = "uses vertex buffer 2/3".into();
        } else {
            // decode
            let attrs: Vec<Attr> = o.attributes.elements.iter().map(|a| Attr { kind: kind_of(a), buf: a.buffer_index as usize, off: a.buffer_offset as usize, ty: a.data_type }).collect();
            let mut vals = vec![];
            for a in &attrs {
                let st = stride_of(&o, a.buf);
                let base = vboff_of(&o, a.buf);
                let b = &m.vertex_buffers.elements[a.buf].elements;
                let (_, nb) = comps(a.ty);
                vals.push((0..nv).map(|i| decode(a.ty, &b[base + i * st + a.off..base + i * st + a.off + nb])).collect::<Vec<_>>());
            }
            let weights = rig_i.map(|r| read_weights(&m.rigging_buffers.elements[r], nv)).unwrap_or_default();
            let max_infl = rig_i.map(|r| m.rigging_buffers.elements[r].flags.max_influences as usize).unwrap_or(0);
            let mut tris: Vec<[u32; 3]> = idx.chunks(3).map(|c| [c[0], c[1], c[2]]).collect();
            // Pokemon LOD1/LOD2 bodies list every triangle k times in a row (same order). Refine the unique set (the
            // repeats would make every edge non-manifold, so nothing would curve) and repeat each refined triangle k times.
            let rep_k = (2..=4).rev().find(|&k| tris.len() % k == 0 && tris.chunks(k).all(|c| c.iter().all(|t| *t == c[0]))).unwrap_or(1);
            if rep_k > 1 {
                tris = tris.chunks(rep_k).map(|c| c[0]).collect();
            }
            let mut ob = Obj { attrs, vals, weights, tris, max_infl };
            let t0 = ob.tris.len() * rep_k;
            let mut rep = Report { v0: nv, t0, v1: 0, t1: 0, curved: 0, straight: 0, seam_weight_mismatch: 0, max_disp_ratio: 0.0 };
            let mut lv = 0;
            while lv < opt.levels {
                // vertex count after this level is at most V + E <= V + 3T/2; stay inside u16 indices
                if ob.vals[0].len() + ob.tris.len() * 3 / 2 + 1 > 65535 {
                    break;
                }
                let (c, s, mm, r) = ob.refine_level(opt.clamp);
                if lv == 0 {
                    rep.curved = c;
                    rep.straight = s;
                    rep.seam_weight_mismatch = mm;
                }
                rep.max_disp_ratio = rep.max_disp_ratio.max(r);
                lv += 1;
            }
            if rep_k > 1 {
                ob.tris = ob.tris.iter().flat_map(|t| std::iter::repeat(*t).take(rep_k)).collect();
            }
            if lv == 0 {
                why = "too many vertices for u16 indices".into();
            } else {
                rep.v1 = ob.vals[0].len();
                rep.t1 = ob.tris.len();
                lines.push(format!(
                    "{label}: levels={lv} verts {} -> {} tris {} -> {} curved_edges={} straight_edges={} seam_weight_mismatch={} max_disp/edge={:.3}",
                    rep.v0, rep.v1, rep.t0, rep.t1, rep.curved, rep.straight, rep.seam_weight_mismatch, rep.max_disp_ratio
                ));
                obj = Some(ob);
            }
        }
        // write this object's vertex data, indices and rigging
        let o_mut = &mut m.objects.elements[oi];
        let vbs = [new_vb[0].len(), new_vb[1].len(), 0, 0];
        match obj {
            None => {
                if want {
                    lines.push(format!("{label}: kept ({why})"));
                }
                for b in 0..2 {
                    let st = stride_of(&o, b);
                    let base = vboff_of(&o, b);
                    if st > 0 {
                        new_vb[b].extend_from_slice(&m.vertex_buffers.elements[b].elements[base..base + st * nv]);
                    }
                }
                let isz = if o.draw_element_type == DrawElementType::UnsignedShort { 2 } else { 4 };
                let off = o.index_buffer_offset as usize;
                let ib_start = new_ib.len();
                new_ib.extend_from_slice(&m.index_buffer.elements[off..off + isz * o.vertex_index_count as usize]);
                o_mut.index_buffer_offset = ib_start as u32;
            }
            Some(ob) => {
                let nv1 = ob.vals[0].len();
                for b in 0..2 {
                    let st = stride_of(&o, b);
                    if st == 0 {
                        continue;
                    }
                    let base = vboff_of(&o, b);
                    new_vb[b].extend_from_slice(&m.vertex_buffers.elements[b].elements[base..base + st * nv]); // original vertices, raw
                    let start = new_vb[b].len();
                    new_vb[b].resize(start + st * (nv1 - nv), 0);
                    for (ai, a) in ob.attrs.iter().enumerate() {
                        if a.buf != b {
                            continue;
                        }
                        let (_, nb) = comps(a.ty);
                        for i in nv..nv1 {
                            let p = start + (i - nv) * st + a.off;
                            encode(a.ty, ob.vals[ai][i], &mut new_vb[b][p..p + nb]);
                        }
                    }
                }
                let mut ix: Vec<u32> = vec![];
                ix.extend_from_slice(&HDR);
                ix.extend(idx.iter().copied());
                ix.extend_from_slice(&SEP);
                for t in &ob.tris {
                    ix.extend_from_slice(t);
                }
                if (ix.len() + TRL.len()) % 2 == 1 {
                    ix.extend_from_slice(&[0, 0, 0]); // even index count keeps the next object 4-byte aligned
                }
                ix.extend_from_slice(&TRL);
                let ib_start = new_ib.len();
                for &x in &ix {
                    new_ib.extend_from_slice(&(x as u16).to_le_bytes());
                }
                o_mut.index_buffer_offset = ib_start as u32;
                o_mut.draw_element_type = DrawElementType::UnsignedShort;
                o_mut.vertex_count = nv1 as u32;
                o_mut.vertex_index_count = ix.len() as u32;
                if let Some(r) = rig_i {
                    let rig = &mut m.rigging_buffers.elements[r];
                    for (slot, bb) in rig.buffers.elements.iter_mut().enumerate() {
                        for i in nv..nv1 {
                            for &(j, w) in &ob.weights[i] {
                                if j as usize == slot && w > 0.0 {
                                    bb.data.elements.extend_from_slice(&(i as u16).to_le_bytes());
                                    bb.data.elements.extend_from_slice(&w.to_le_bytes());
                                }
                            }
                        }
                    }
                }
            }
        }
        let _ = vbs;
        while new_ib.len() % 4 != 0 {
            new_ib.push(0); // next object's index range starts 4-byte aligned, as in the game's files
        }
    }
    // offsets as running sums
    let mut vb = [0usize; 4];
    for o in m.objects.elements.iter_mut() {
        o.vertex_buffer0_offset = vb[0] as u32;
        o.vertex_buffer1_offset = vb[1] as u32;
        o.vertex_buffer2_offset = vb[2] as u32;
        o.vertex_buffer3_offset = 0;
        let n = o.vertex_count as usize;
        vb[0] += o.stride0 as usize * n;
        vb[1] += o.stride1 as usize * n;
        vb[2] += 32 * n;
    }
    for b in 0..2 {
        if b < m.vertex_buffers.elements.len() {
            m.vertex_buffers.elements[b].elements = std::mem::take(&mut new_vb[b]);
        }
    }
    for (b, s) in m.buffer_sizes.elements.iter_mut().enumerate() {
        *s = m.vertex_buffers.elements.get(b).map(|x| x.elements.len() as u32).unwrap_or(0);
    }
    m.index_buffer.elements = new_ib;
    m.polygon_index_size = m.index_buffer.elements.len() as u64;
    Ok(lines)
}
