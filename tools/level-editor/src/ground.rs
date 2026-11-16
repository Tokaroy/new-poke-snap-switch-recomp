//! Terrain editing (owner list item 2). One height grid and one layer grid cover the course's ground models. Brushes
//! write grid cells through the undo history (pseudo levels `LEVEL_HEIGHT` and `LEVEL_LAYER`, offset = cell index); the
//! preview and the export read the grids, so the ground mesh, its collision and its navmesh always move together.
//! Export patches the original files in place (layouts, counts and indices never change); see `terrain.rs` for formats.

use crate::course::LoadedCourse;
use crate::pick::Ground;
use crate::terrain::{displace_collision, f32_at, find_tiles, parse_collision, put_f32, vec3_at, Shape};
use glam::{Mat3, Mat4, Quat, Vec3};
use std::path::{Path, PathBuf};

/// Pseudo level indices for undo writes: offset = cell index.
pub const LEVEL_HEIGHT: usize = usize::MAX;
pub const LEVEL_LAYER: usize = usize::MAX - 1;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Op {
    Raise,
    Lower,
    Smooth,
    Flatten,
    /// Raises the ground mesh's layer blend (vertex color Color1 RGB).
    Paint,
    Erase,
}

impl Op {
    pub const ALL: [Op; 6] = [Op::Raise, Op::Lower, Op::Smooth, Op::Flatten, Op::Paint, Op::Erase];
    pub fn label(self) -> &'static str {
        match self {
            Op::Raise => "Raise",
            Op::Lower => "Lower",
            Op::Smooth => "Smooth",
            Op::Flatten => "Flatten",
            Op::Paint => "Paint layer",
            Op::Erase => "Erase layer",
        }
    }
    pub fn tip(self) -> &'static str {
        match self {
            Op::Raise => "Lifts the ground under the brush, most in the middle.",
            Op::Lower => "Sinks the ground under the brush, most in the middle.",
            Op::Smooth => "Evens out bumps and dents under the brush.",
            Op::Flatten => "Pulls the ground toward the height where the stroke started.",
            Op::Paint => "Paints the ground's top texture layer over the one below (on Florio: grass over the dirt path). It is the blend the course artists painted into the ground's vertex colors.",
            Op::Erase => "Wipes the top texture layer away so the one below shows (on Florio: dirt where there was grass).",
        }
    }
    pub fn is_layer(self) -> bool {
        matches!(self, Op::Paint | Op::Erase)
    }
    pub fn parse(s: &str) -> Option<Op> {
        Op::ALL.into_iter().find(|o| o.label().split(' ').next().unwrap_or("").eq_ignore_ascii_case(s))
    }
}

pub struct Terrain {
    pub x0: f32,
    pub z0: f32,
    pub cell: f32,
    pub nx: usize,
    pub nz: usize,
    /// Original ground height at each grid point (NaN: no ground there).
    pub base: Vec<f32>,
    pub delta: Vec<f32>,
    pub layer: Vec<f32>,
    /// Model indices (into `LoadedCourse::models`) that are terrain.
    pub models: Vec<usize>,
}

pub fn is_terrain_name(name: &str) -> bool {
    let n = name.to_lowercase();
    n.contains("ground") && !n.contains("water")
}

fn smooth01(t: f32) -> f32 {
    let t = t.clamp(0.0, 1.0);
    t * t * (3.0 - 2.0 * t)
}

/// How much a collision or navmesh point at height `y` follows the ground whose original height is `base`: fully when it
/// lies on the ground, not at all when it is clearly above or below it (rock tops, overhangs, a cave floor).
pub fn follow_weight(y: f32, base: f32) -> f32 {
    if base.is_nan() {
        return 0.0;
    }
    1.0 - smooth01(((y - base).abs() - 1.0) / 1.5)
}

pub fn inst_xf(i: &crate::level::ModelInstance) -> Mat4 {
    let q = Quat::from_array(i.rot);
    let q = if q.length_squared() < 1e-6 { Quat::IDENTITY } else { q.normalize() };
    Mat4::from_scale_rotation_translation(Vec3::from(i.scale), q, Vec3::from(i.pos))
}

impl Terrain {
    /// Grid over the course's ground models with heights from the (unedited) ground geometry.
    pub fn build(c: &LoadedCourse, g: &Ground) -> Option<Terrain> {
        let models: Vec<usize> = c
            .models
            .iter()
            .enumerate()
            .filter(|(i, m)| m.inst.alive && m.mesh.is_some() && is_terrain_name(&m.inst.name) && g.model_bounds(*i).is_some())
            .map(|(i, _)| i)
            .collect();
        if models.is_empty() {
            return None;
        }
        let (mut mn, mut mx) = (Vec3::splat(f32::MAX), Vec3::splat(f32::MIN));
        for &m in &models {
            let (a, b) = g.model_bounds(m)?;
            mn = mn.min(a);
            mx = mx.max(b);
        }
        let span = (mx.x - mn.x).max(mx.z - mn.z);
        let cell = (span / 400.0).max(1.0);
        let nx = ((mx.x - mn.x) / cell).ceil() as usize + 2;
        let nz = ((mx.z - mn.z) / cell).ceil() as usize + 2;
        let mut base = vec![f32::NAN; nx * nz];
        let keep = |m: usize| models.contains(&m);
        for iz in 0..nz {
            for ix in 0..nx {
                let p = Vec3::new(mn.x + ix as f32 * cell, mx.y + 10.0, mn.z + iz as f32 * cell);
                if let Some(h) = g.raycast_where(p, -Vec3::Y, mx.y - mn.y + 20.0, &keep) {
                    base[iz * nx + ix] = h.point.y;
                }
            }
        }
        Some(Terrain { x0: mn.x, z0: mn.z, cell, nx, nz, base, delta: vec![0.0; nx * nz], layer: vec![0.0; nx * nz], models })
    }

    pub fn point(&self, i: usize) -> (f32, f32) {
        (self.x0 + (i % self.nx) as f32 * self.cell, self.z0 + (i / self.nx) as f32 * self.cell)
    }

    pub fn sample(&self, f: &[f32], x: f32, z: f32) -> f32 {
        let gx = ((x - self.x0) / self.cell).clamp(0.0, (self.nx - 1) as f32);
        let gz = ((z - self.z0) / self.cell).clamp(0.0, (self.nz - 1) as f32);
        let ix = (gx as usize).min(self.nx.saturating_sub(2));
        let iz = (gz as usize).min(self.nz.saturating_sub(2));
        let (tx, tz) = (gx - ix as f32, gz - iz as f32);
        let at = |a: usize, b: usize| {
            let v = f[(iz + b).min(self.nz - 1) * self.nx + (ix + a).min(self.nx - 1)];
            if v.is_nan() {
                0.0
            } else {
                v
            }
        };
        let top = at(0, 0) * (1.0 - tx) + at(1, 0) * tx;
        let bot = at(0, 1) * (1.0 - tx) + at(1, 1) * tx;
        top * (1.0 - tz) + bot * tz
    }

    /// Height change at (x, z).
    pub fn dy(&self, x: f32, z: f32) -> f32 {
        self.sample(&self.delta, x, z)
    }
    pub fn layer_at(&self, x: f32, z: f32) -> f32 {
        self.sample(&self.layer, x, z)
    }
    /// Original ground height at (x, z); NaN when the nearest grid point has no ground.
    pub fn base_at(&self, x: f32, z: f32) -> f32 {
        let ix = ((x - self.x0) / self.cell).round();
        let iz = ((z - self.z0) / self.cell).round();
        if ix < 0.0 || iz < 0.0 || ix as usize >= self.nx || iz as usize >= self.nz {
            return f32::NAN;
        }
        let b = self.base[iz as usize * self.nx + ix as usize];
        if b.is_nan() {
            return b;
        }
        // bilinear between neighbors when they agree (no cliff edge in between)
        let s = self.sample(&self.base, x, z);
        if (s - b).abs() < 2.0 {
            s
        } else {
            b
        }
    }
    /// Slope of the height change (d/dx, d/dz).
    pub fn grad(&self, x: f32, z: f32) -> (f32, f32) {
        let h = self.cell * 0.5;
        ((self.dy(x + h, z) - self.dy(x - h, z)) / (2.0 * h), (self.dy(x, z + h) - self.dy(x, z - h)) / (2.0 * h))
    }
    pub fn edited(&self) -> bool {
        self.delta.iter().any(|d| d.abs() > 1e-6) || self.layer.iter().any(|d| d.abs() > 1e-6)
    }
    pub fn surface(&self, i: usize) -> f32 {
        self.base[i] + self.delta[i]
    }
    /// Current ground height at (x, z) (original plus edits).
    pub fn height_at(&self, x: f32, z: f32) -> f32 {
        self.base_at(x, z) + self.dy(x, z)
    }

    /// One brush dab: the cell writes it makes (pseudo level, cell, new value). `flat_y` is the Flatten target.
    pub fn dab(&self, op: Op, center: Vec3, radius: f32, strength: f32, flat_y: f32) -> Vec<(usize, usize, f32)> {
        let mut out = vec![];
        let r = radius.max(self.cell);
        let ix0 = ((center.x - r - self.x0) / self.cell).floor().max(0.0) as usize;
        let iz0 = ((center.z - r - self.z0) / self.cell).floor().max(0.0) as usize;
        let ix1 = (((center.x + r - self.x0) / self.cell).ceil().max(0.0) as usize).min(self.nx - 1);
        let iz1 = (((center.z + r - self.z0) / self.cell).ceil().max(0.0) as usize).min(self.nz - 1);
        let s = strength.clamp(0.0, 1.0);
        for iz in iz0..=iz1 {
            for ix in ix0..=ix1 {
                let i = iz * self.nx + ix;
                if self.base[i].is_nan() {
                    continue;
                }
                let (x, z) = self.point(i);
                let d = ((x - center.x).powi(2) + (z - center.z).powi(2)).sqrt();
                if d > r {
                    continue;
                }
                let f = smooth01(1.0 - d / r);
                let w = match op {
                    Op::Raise => (LEVEL_HEIGHT, self.delta[i] + 0.3 * s * f),
                    Op::Lower => (LEVEL_HEIGHT, self.delta[i] - 0.3 * s * f),
                    Op::Smooth => {
                        let (mut sum, mut n) = (0.0, 0);
                        for (dx, dz) in [(-1i64, 0i64), (1, 0), (0, -1), (0, 1), (-1, -1), (1, 1), (-1, 1), (1, -1)] {
                            let (jx, jz) = (ix as i64 + dx, iz as i64 + dz);
                            if jx < 0 || jz < 0 || jx as usize >= self.nx || jz as usize >= self.nz {
                                continue;
                            }
                            let j = jz as usize * self.nx + jx as usize;
                            if !self.base[j].is_nan() {
                                sum += self.surface(j);
                                n += 1;
                            }
                        }
                        if n == 0 {
                            continue;
                        }
                        let avg = sum / n as f32;
                        (LEVEL_HEIGHT, self.delta[i] + 0.5 * s * f * (avg - self.surface(i)))
                    }
                    Op::Flatten => (LEVEL_HEIGHT, self.delta[i] + 0.5 * s * f * (flat_y - self.surface(i))),
                    Op::Paint => (LEVEL_LAYER, (self.layer[i] + 0.25 * s * f).min(1.0)),
                    Op::Erase => (LEVEL_LAYER, (self.layer[i] - 0.25 * s * f).max(-1.0)),
                };
                out.push((w.0, i, w.1));
            }
        }
        out
    }

    pub fn get(&self, level: usize, i: usize) -> Option<f32> {
        match level {
            LEVEL_HEIGHT => self.delta.get(i).copied(),
            LEVEL_LAYER => self.layer.get(i).copied(),
            _ => None,
        }
    }
    pub fn set(&mut self, level: usize, i: usize, v: f32) {
        let f = if level == LEVEL_HEIGHT { &mut self.delta } else { &mut self.layer };
        if let Some(x) = f.get_mut(i) {
            *x = v;
        }
    }

    /// Writes the edited ground files of every terrain model into `arch` (the pack's copy of the course folder): the mesh
    /// (positions, normals, tangent frame, layer blend), the collision and the navmesh. Nothing when nothing was edited.
    pub fn export(&self, c: &LoadedCourse, arch: &Path) -> Result<Vec<PathBuf>, String> {
        let mut written = vec![];
        if !self.edited() {
            return Ok(written);
        }
        for &mi in &self.models {
            let m = &c.models[mi];
            let Some(src) = &m.source else { continue };
            let dir = src.parent().ok_or("model without folder")?;
            if !dir.to_string_lossy().replace('\\', "/").to_lowercase().ends_with(&c.archive.to_lowercase()) {
                return Err(format!("{}: ground model is outside the course pack; editing it is not supported", m.inst.name));
            }
            for (path, bytes) in self.ground_files(dir, &m.inst.name, src, inst_xf(&m.inst))? {
                let out = arch.join(path.file_name().ok_or("bad file name")?);
                std::fs::write(&out, &bytes).map_err(|e| format!("{}: {e}", out.display()))?;
                written.push(out);
            }
        }
        Ok(written)
    }

    /// The edited files of one ground model: (original path, new bytes).
    pub fn ground_files(&self, dir: &Path, name: &str, modl: &Path, inst: Mat4) -> Result<Vec<(PathBuf, Vec<u8>)>, String> {
        let mut out = vec![];
        let weight = |p: Vec3| follow_weight(p.y, self.base_at(p.x, p.z));
        let modl_data = ssbh_data::prelude::ModlData::from_file(modl).map_err(|e| format!("{}: {e}", modl.display()))?;
        let mesh_path = dir.join(&modl_data.mesh_file_name);
        let skel = ssbh_data::prelude::SkelData::from_file(dir.join(&modl_data.skeleton_file_name)).ok();
        out.push((mesh_path.clone(), self.patch_mesh(&mesh_path, inst, skel.as_ref())?));
        // collision: only the plain ground pieces (no rotation or scale) follow; rocks and props keep their shape
        let col_path = dir.join(format!("{name}.DC4A8177"));
        if let Ok(mut b) = std::fs::read(&col_path) {
            let shapes = parse_collision(&b).map_err(|e| format!("{}: {e}", col_path.display()))?;
            let plain: Vec<Shape> = shapes.into_iter().filter(|s| (s.scale - Vec3::ONE).length() < 1e-3 && s.rot.length() < 1e-3).collect();
            displace_collision(&mut b, &plain, inst, &|x, z| self.dy(x, z), &weight);
            out.push((col_path, b));
        }
        let nav_path = dir.join(format!("{name}.navmshb"));
        if let Ok(mut b) = std::fs::read(&nav_path) {
            for t in find_tiles(&b) {
                t.displace(&mut b, &|p| self.dy(p.x, p.z) * weight(p));
            }
            out.push((nav_path, b));
        }
        Ok(out)
    }

    /// Moves mesh vertices with the ground, turns normals and tangent frames with the added slope and adds the layer
    /// offset to the Color1 RGB. The file keeps its layout; bounds only grow.
    pub fn patch_mesh(&self, path: &Path, inst: Mat4, skel: Option<&ssbh_data::prelude::SkelData>) -> Result<Vec<u8>, String> {
        use ssbh_lib::formats::mesh::*;
        let mesh = Mesh::from_file(path).map_err(|e| format!("{}: {e}", path.display()))?;
        let Mesh::V10(mut m) = mesh else { return Err(format!("{}: not a version 1.10 mesh", path.display())) };
        let mut all = (Vec3::splat(f32::MAX), Vec3::splat(f32::MIN));
        for oi in 0..m.objects.elements.len() {
            let o = m.objects.elements[oi].clone();
            let bone = if o.use_vertex_skinning == 0 {
                let bn = o.parent_bone_name.to_str().unwrap_or("").to_string();
                skel.and_then(|s| s.bones.iter().find(|b| b.name == bn).and_then(|b| s.calculate_world_transform(b).ok()))
                    .unwrap_or(Mat4::IDENTITY)
            } else {
                Mat4::IDENTITY
            };
            let xf = inst * bone;
            let inv = xf.inverse();
            let nm = Mat3::from_mat4(xf);
            let n_to_world = nm.inverse().transpose();
            let n_to_local = nm.transpose();
            let stride = [o.stride0, o.stride1, o.stride2, o.stride3];
            let base_off = [o.vertex_buffer0_offset, o.vertex_buffer1_offset, o.vertex_buffer2_offset, o.vertex_buffer3_offset];
            let attr = |u: AttributeUsageV9, name: Option<&str>| -> Option<AttributeV10> {
                o.attributes
                    .elements
                    .iter()
                    .find(|a| {
                        a.usage == u
                            && name.map_or(true, |n| {
                                a.name.to_str() == Some(n) || a.attribute_names.elements.iter().any(|s| s.to_str() == Some(n))
                            })
                    })
                    .cloned()
            };
            let Some(pos) = attr(AttributeUsageV9::Position, None).filter(|a| a.data_type == AttributeDataTypeV10::Float3) else { continue };
            let nrm = attr(AttributeUsageV9::Normal, None);
            let frame: Vec<AttributeV10> = [attr(AttributeUsageV9::Tangent, None), attr(AttributeUsageV9::Binormal, None)].into_iter().flatten().collect();
            let col = attr(AttributeUsageV9::ColorSet, Some("Color1"));
            let at = |a: &AttributeV10, v: usize| {
                let b = a.buffer_index as usize;
                (b, base_off[b] as usize + stride[b] as usize * v + a.buffer_offset as usize)
            };
            let float3 = |a: &AttributeV10| matches!(a.data_type, AttributeDataTypeV10::Float3 | AttributeDataTypeV10::Float4);
            let (mut lo, mut hi) = (Vec3::splat(f32::MAX), Vec3::splat(f32::MIN));
            for v in 0..o.vertex_count as usize {
                let (pb, pq) = at(&pos, v);
                let Some(p) = m.vertex_buffers.elements.get(pb).and_then(|b| vec3_at(&b.elements, pq)) else { continue };
                let w = xf.transform_point3(p);
                let wt = follow_weight(w.y, self.base_at(w.x, w.z));
                let d = self.dy(w.x, w.z) * wt;
                let np = inv.transform_point3(w + Vec3::Y * d);
                lo = lo.min(np);
                hi = hi.max(np);
                if d != 0.0 {
                    let buf = &mut m.vertex_buffers.elements[pb].elements;
                    for k in 0..3 {
                        put_f32(buf, pq + 4 * k, np[k]);
                    }
                }
                // tilt the shading frame by the slope the edit added
                let (gx, gz) = self.grad(w.x, w.z);
                if let Some(na) = nrm.as_ref().filter(|a| float3(a)) {
                    let (nb, nq) = at(na, v);
                    let n = vec3_at(&m.vertex_buffers.elements[nb].elements, nq).unwrap_or(Vec3::Y);
                    let nw = (n_to_world * n).normalize_or_zero();
                    if (gx != 0.0 || gz != 0.0) && wt > 0.0 && nw.y > 0.05 {
                        let nn = Vec3::new(nw.x - gx * wt * nw.y, nw.y, nw.z - gz * wt * nw.y).normalize();
                        let nl = (n_to_local * nn).normalize();
                        let buf = &mut m.vertex_buffers.elements[nb].elements;
                        for k in 0..3 {
                            put_f32(buf, nq + 4 * k, nl[k]);
                        }
                        for a in frame.iter().filter(|a| float3(a)) {
                            let (tb, tq) = at(a, v);
                            let buf = &mut m.vertex_buffers.elements[tb].elements;
                            let t = vec3_at(buf, tq).unwrap_or(Vec3::ZERO);
                            let t2 = (t - nl * nl.dot(t)).normalize_or_zero() * t.length();
                            if t2 != Vec3::ZERO {
                                for k in 0..3 {
                                    put_f32(buf, tq + 4 * k, t2[k]);
                                }
                            }
                        }
                    }
                }
                if let Some(ca) = &col {
                    let l = self.layer_at(w.x, w.z);
                    if l != 0.0 {
                        let (cb, cq) = at(ca, v);
                        let buf = &mut m.vertex_buffers.elements[cb].elements;
                        // the blend is Color1 RGB (in game, 1 = top layer, e.g. Florio's grass; 0 = the dirt below).
                        // Alpha changes nothing visible (run 0-20261115_230308) and is left as it was.
                        for k in 0..3 {
                            match ca.data_type {
                                AttributeDataTypeV10::Byte4 => {
                                    let a = buf[cq + k] as f32 / 255.0;
                                    buf[cq + k] = ((a + l).clamp(0.0, 1.0) * 255.0).round() as u8;
                                }
                                AttributeDataTypeV10::Float4 => {
                                    let a = f32_at(buf, cq + 4 * k).unwrap_or(0.0);
                                    put_f32(buf, cq + 4 * k, (a + l).clamp(0.0, 1.0));
                                }
                                AttributeDataTypeV10::HalfFloat4 => {
                                    let a = half::f16::from_le_bytes([buf[cq + 2 * k], buf[cq + 2 * k + 1]]).to_f32();
                                    buf[cq + 2 * k..cq + 2 * k + 2].copy_from_slice(&half::f16::from_f32((a + l).clamp(0.0, 1.0)).to_le_bytes());
                                }
                                _ => {}
                            }
                        }
                    }
                }
            }
            if lo.x <= hi.x {
                grow_bounds(&mut m.objects.elements[oi].bounding_info, lo, hi);
                all = (all.0.min(lo), all.1.max(hi));
            }
        }
        if all.0.x <= all.1.x {
            grow_bounds(&mut m.bounding_info, all.0, all.1);
        }
        let mut cur = std::io::Cursor::new(Vec::new());
        Mesh::V10(m).write(&mut cur).map_err(|e| format!("{}: {e}", path.display()))?;
        Ok(cur.into_inner())
    }
}

/// Grows a mesh bounding sphere, box and oriented box so they hold the box lo..hi (never shrinks them). Unchanged when the
/// box already fits, so an unedited mesh writes back byte for byte.
fn grow_bounds(b: &mut ssbh_lib::formats::mesh::BoundingInfo, lo: Vec3, hi: Vec3) {
    use ssbh_lib::{Matrix3x3, Vector3};
    let v3 = |p: Vec3| Vector3 { x: p.x, y: p.y, z: p.z };
    let bv = &mut b.bounding_volume;
    let (a0, a1) = (Vec3::new(bv.min.x, bv.min.y, bv.min.z), Vec3::new(bv.max.x, bv.max.y, bv.max.z));
    if lo.cmpge(a0 - 1e-4).all() && hi.cmple(a1 + 1e-4).all() {
        return;
    }
    let (n0, n1) = (a0.min(lo), a1.max(hi));
    bv.min = v3(n0);
    bv.max = v3(n1);
    let s = &mut b.bounding_sphere;
    let c = Vec3::new(s.center.x, s.center.y, s.center.z);
    let mut far = 0.0f32;
    for k in 0..8 {
        let p = Vec3::new(if k & 1 == 0 { n0.x } else { n1.x }, if k & 2 == 0 { n0.y } else { n1.y }, if k & 4 == 0 { n0.z } else { n1.z });
        far = far.max(p.distance(c));
    }
    s.radius = s.radius.max(far);
    // oriented box: replaced by the axis-aligned box when the new box no longer fits (sizes are full extents)
    let o = &mut b.oriented_bounding_box;
    let oc = Vec3::new(o.center.x, o.center.y, o.center.z);
    let half = Vec3::new(o.size.x, o.size.y, o.size.z) * 0.5;
    let axis_aligned = (o.transform.col1.x - 1.0).abs() < 1e-4 && (o.transform.col2.y - 1.0).abs() < 1e-4 && (o.transform.col3.z - 1.0).abs() < 1e-4;
    if !(axis_aligned && n0.cmpge(oc - half).all() && n1.cmple(oc + half).all()) {
        o.center = v3((n0 + n1) * 0.5);
        o.size = v3(n1 - n0);
        o.transform = Matrix3x3 { col1: v3(Vec3::X), col2: v3(Vec3::Y), col3: v3(Vec3::Z) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn flat(n: usize) -> Terrain {
        Terrain { x0: 0.0, z0: 0.0, cell: 1.0, nx: n, nz: n, base: vec![0.0; n * n], delta: vec![0.0; n * n], layer: vec![0.0; n * n], models: vec![] }
    }
    fn apply(t: &mut Terrain, w: Vec<(usize, usize, f32)>) {
        for (l, i, v) in w {
            t.set(l, i, v);
        }
    }

    #[test]
    fn brushes_shape_the_ground() {
        let mut t = flat(21);
        let c = Vec3::new(10.0, 0.0, 10.0);
        for _ in 0..10 {
            let w = t.dab(Op::Raise, c, 5.0, 1.0, 0.0);
            apply(&mut t, w);
        }
        let mid = t.dy(10.0, 10.0);
        assert!((mid - 3.0).abs() < 1e-3, "raised {mid}");
        assert_eq!(t.dy(0.0, 0.0), 0.0, "outside the brush stays");
        assert!(t.dy(13.0, 10.0) > 0.0 && t.dy(13.0, 10.0) < mid, "falloff");
        for _ in 0..20 {
            let w = t.dab(Op::Smooth, c, 6.0, 1.0, 0.0);
            apply(&mut t, w);
        }
        assert!(t.dy(10.0, 10.0) < mid * 0.9, "smooth lowers the peak");
        for _ in 0..40 {
            let w = t.dab(Op::Flatten, c, 8.0, 1.0, 0.5);
            apply(&mut t, w);
        }
        assert!((t.dy(10.0, 10.0) - 0.5).abs() < 0.05, "flatten reaches the target: {}", t.dy(10.0, 10.0));
        let w = t.dab(Op::Paint, c, 3.0, 1.0, 0.0);
        apply(&mut t, w);
        assert!(t.layer_at(10.0, 10.0) > 0.2 && t.layer_at(0.0, 0.0) == 0.0);
    }

    #[test]
    fn follow_weight_keeps_rock_tops() {
        assert_eq!(follow_weight(0.2, 0.0), 1.0);
        assert_eq!(follow_weight(4.0, 0.0), 0.0);
        assert_eq!(follow_weight(0.0, f32::NAN), 0.0);
    }
}
