//! Hiding a course's own scenery inside an area (the random course's rail corridor).
//!
//! The course's scenery is baked in world space: one model group (TreeA, Rock2, ...) holds many trees or rocks, one mesh
//! object each, under a single instance at the origin. Hiding one tree therefore means changing the mesh, not the
//! level. Each mesh object whose bounds lie inside the area has all its vertices moved onto its center, so it draws
//! nothing; every other byte of the file stays as it was (layout, materials, bounds, LOD files). The rock shapes of the
//! ground collision inside the area are collapsed the same way, so Pokémon do not bump into rocks nobody can see.

use crate::terrain::{parse_collision, put_f32, vec3_at};
use glam::{Mat4, Vec3};
use std::path::Path;

/// Inside test on an object's world bounds (lowest and highest corner).
pub type Inside<'a> = &'a dyn Fn(Vec3, Vec3) -> bool;

/// One mesh file with every object inside the area collapsed to a point. Returns the new bytes and the number of
/// objects hidden, or None when nothing is inside.
pub fn collapse_mesh(path: &Path, inst: Mat4, inside: Inside) -> Result<Option<(Vec<u8>, usize)>, String> {
    use ssbh_lib::formats::mesh::*;
    let mesh = Mesh::from_file(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let Mesh::V10(mut m) = mesh else { return Err(format!("{}: not a version 1.10 mesh", path.display())) };
    let inv = inst.inverse();
    let mut hidden = 0;
    for oi in 0..m.objects.elements.len() {
        let o = m.objects.elements[oi].clone();
        let Some(pos) = o.attributes.elements.iter().find(|a| a.usage == AttributeUsageV9::Position && a.data_type == AttributeDataTypeV10::Float3).cloned() else { continue };
        let stride = [o.stride0, o.stride1, o.stride2, o.stride3];
        let base = [o.vertex_buffer0_offset, o.vertex_buffer1_offset, o.vertex_buffer2_offset, o.vertex_buffer3_offset];
        let b = pos.buffer_index as usize;
        let at = |v: usize| base[b] as usize + stride[b] as usize * v + pos.buffer_offset as usize;
        let Some(buf) = m.vertex_buffers.elements.get(b) else { continue };
        let (mut lo, mut hi) = (Vec3::splat(f32::MAX), Vec3::splat(f32::MIN));
        for v in 0..o.vertex_count as usize {
            if let Some(p) = vec3_at(&buf.elements, at(v)) {
                let w = inst.transform_point3(p);
                lo = lo.min(w);
                hi = hi.max(w);
            }
        }
        if lo.x > hi.x || !inside(lo, hi) {
            continue;
        }
        let c = inv.transform_point3((lo + hi) * 0.5);
        let buf = &mut m.vertex_buffers.elements[b].elements;
        for v in 0..o.vertex_count as usize {
            let q = at(v);
            for k in 0..3 {
                put_f32(buf, q + 4 * k, c[k]);
            }
        }
        hidden += 1;
    }
    if hidden == 0 {
        return Ok(None);
    }
    let mut out = std::io::Cursor::new(vec![]);
    Mesh::V10(m).write(&mut out).map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(Some((out.into_inner(), hidden)))
}

/// The mesh files of a model: its LOD0 mesh and the LOD files beside it (`<mesh>_LOD1.numshb`, ...).
pub fn mesh_files(dir: &Path, mesh_file: &str) -> Vec<std::path::PathBuf> {
    let stem = mesh_file.trim_end_matches(".numshb");
    let mut out = vec![dir.join(mesh_file)];
    for k in 1..4 {
        let p = dir.join(format!("{stem}_LOD{k}.numshb"));
        if p.exists() {
            out.push(p);
        }
    }
    out
}

/// The ground collision with every rock shape (one with a transform) inside the area collapsed to a point. Returns the
/// new bytes and the number of shapes collapsed. Plain ground pieces are never touched.
pub fn collapse_collision(b: &[u8], inst: Mat4, inside: Inside) -> Result<(Vec<u8>, usize), String> {
    let shapes = parse_collision(b)?;
    let mut out = b.to_vec();
    let mut n = 0;
    for s in shapes.iter().filter(|s| !s.identity() && !s.verts.is_empty()) {
        let xf = inst * s.xf();
        let w: Vec<Vec3> = s.verts.iter().map(|v| xf.transform_point3(*v)).collect();
        let lo = w.iter().fold(Vec3::splat(f32::MAX), |a, v| a.min(*v));
        let hi = w.iter().fold(Vec3::splat(f32::MIN), |a, v| a.max(*v));
        if !inside(lo, hi) {
            continue;
        }
        let c = s.verts.iter().fold(Vec3::ZERO, |a, v| a + *v) / s.verts.len() as f32;
        for k in 0..s.verts.len() {
            for j in 0..3 {
                put_f32(&mut out, s.verts_off + 12 * k + 4 * j, c[j]);
            }
        }
        n += 1;
    }
    Ok((out, n))
}

/// An area around a rail: everything whose ground-plane center lies within `width` of the rail, and that is not wider
/// than `max_size` (a cliff or backdrop spanning the course stays).
#[derive(Clone, Debug)]
pub struct Corridor {
    pub rail: Vec<glam::Vec2>,
    pub width: f32,
    pub max_size: f32,
}

impl Corridor {
    pub fn dist(&self, p: glam::Vec2) -> f32 {
        self.rail
            .windows(2)
            .map(|w| {
                let (a, b) = (w[0], w[1]);
                let ab = b - a;
                let t = ((p - a).dot(ab) / ab.length_squared().max(1e-9)).clamp(0.0, 1.0);
                p.distance(a + ab * t)
            })
            .fold(f32::MAX, f32::min)
    }
    pub fn has_point(&self, p: Vec3) -> bool {
        self.dist(glam::Vec2::new(p.x, p.z)) < self.width
    }
    pub fn holds(&self, lo: Vec3, hi: Vec3) -> bool {
        let size = (hi.x - lo.x).max(hi.z - lo.z);
        size <= self.max_size && self.has_point((lo + hi) * 0.5)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use glam::Vec2;

    #[test]
    fn corridor_holds_small_things_near_the_rail() {
        let c = Corridor { rail: vec![Vec2::new(0.0, 0.0), Vec2::new(100.0, 0.0)], width: 10.0, max_size: 30.0 };
        assert!(c.holds(Vec3::new(48.0, 0.0, 3.0), Vec3::new(52.0, 9.0, 7.0)));
        assert!(!c.holds(Vec3::new(48.0, 0.0, 13.0), Vec3::new(52.0, 9.0, 17.0)));
        assert!(!c.holds(Vec3::new(0.0, 0.0, -5.0), Vec3::new(60.0, 9.0, 5.0)));
        assert!((c.dist(Vec2::new(-3.0, 4.0)) - 5.0).abs() < 1e-5);
    }
}
