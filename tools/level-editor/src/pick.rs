//! CPU picking against the course geometry: click-to-select, drag along the ground, "drop to ground" and the
//! under-ground check. Triangles are kept in world space per model with a bounding box for fast rejection.

use crate::render::OrbitCamera;
use glam::{Vec3, Vec4};

pub struct GroundModel {
    pub model: usize,
    pub tri_start: usize,
    pub tri_end: usize,
    pub min: Vec3,
    pub max: Vec3,
}

#[derive(Default)]
pub struct Ground {
    pub pos: Vec<Vec3>,
    pub idx: Vec<u32>,
    pub models: Vec<GroundModel>,
}

#[derive(Clone, Copy, Debug)]
pub struct Hit {
    pub t: f32,
    pub point: Vec3,
    pub model: usize,
}

fn ray_box(o: Vec3, inv_d: Vec3, min: Vec3, max: Vec3, tmax: f32) -> bool {
    let t1 = (min - o) * inv_d;
    let t2 = (max - o) * inv_d;
    let tn = t1.min(t2).max_element();
    let tf = t1.max(t2).min_element();
    tf >= tn.max(0.0) && tn <= tmax
}

/// Möller-Trumbore, two-sided.
fn ray_tri(o: Vec3, d: Vec3, a: Vec3, b: Vec3, c: Vec3) -> Option<f32> {
    let e1 = b - a;
    let e2 = c - a;
    let p = d.cross(e2);
    let det = e1.dot(p);
    if det.abs() < 1e-9 {
        return None;
    }
    let inv = 1.0 / det;
    let s = o - a;
    let u = s.dot(p) * inv;
    if !(0.0..=1.0).contains(&u) {
        return None;
    }
    let q = s.cross(e1);
    let v = d.dot(q) * inv;
    if v < 0.0 || u + v > 1.0 {
        return None;
    }
    let t = e2.dot(q) * inv;
    (t > 1e-4).then_some(t)
}

impl Ground {
    /// Nearest hit along the ray within `tmax`, skipping model `skip` (the one being dragged).
    pub fn raycast(&self, o: Vec3, d: Vec3, tmax: f32, skip: Option<usize>) -> Option<Hit> {
        let inv_d = Vec3::ONE / d;
        let mut best: Option<Hit> = None;
        for m in &self.models {
            if Some(m.model) == skip {
                continue;
            }
            let lim = best.map(|b| b.t).unwrap_or(tmax);
            if !ray_box(o, inv_d, m.min, m.max, lim) {
                continue;
            }
            for t in m.tri_start..m.tri_end {
                let i = 3 * t;
                let (a, b, c) =
                    (self.pos[self.idx[i] as usize], self.pos[self.idx[i + 1] as usize], self.pos[self.idx[i + 2] as usize]);
                if let Some(tt) = ray_tri(o, d, a, b, c) {
                    if tt < best.map(|b| b.t).unwrap_or(tmax) {
                        best = Some(Hit { t: tt, point: o + d * tt, model: m.model });
                    }
                }
            }
        }
        best
    }

    /// Height of the first surface at or below `p` (searching from slightly above it).
    pub fn below(&self, p: Vec3, skip: Option<usize>) -> Option<Vec3> {
        self.raycast(p + Vec3::Y * 0.5, -Vec3::Y, 500.0, skip).map(|h| h.point)
    }

    /// True when nothing is below the point but there is a surface above it: the point sits under the terrain.
    pub fn is_under(&self, p: Vec3) -> bool {
        self.raycast(p + Vec3::Y * 0.3, -Vec3::Y, 200.0, None).is_none()
            && self.raycast(p, Vec3::Y, 200.0, None).is_some()
    }

    pub fn model_bounds(&self, model: usize) -> Option<(Vec3, Vec3)> {
        self.models.iter().find(|m| m.model == model).map(|m| (m.min, m.max))
    }
}

/// World-space ray through a screen position inside `rect` (egui points).
pub fn screen_ray(cam: &OrbitCamera, rect: egui::Rect, at: egui::Pos2) -> (Vec3, Vec3) {
    let inv = cam.view_proj(rect.width() / rect.height().max(1.0)).inverse();
    let nx = (at.x - rect.min.x) / rect.width() * 2.0 - 1.0;
    let ny = 1.0 - (at.y - rect.min.y) / rect.height() * 2.0;
    let near = inv.project_point3(Vec3::new(nx, ny, 0.0));
    let far = inv.project_point3(Vec3::new(nx, ny, 1.0));
    (near, (far - near).normalize_or_zero())
}

/// Screen position of a world point, or None when it is behind the camera.
pub fn project(cam: &OrbitCamera, rect: egui::Rect, p: Vec3) -> Option<egui::Pos2> {
    let c = cam.view_proj(rect.width() / rect.height().max(1.0)) * Vec4::new(p.x, p.y, p.z, 1.0);
    if c.w <= 1e-4 {
        return None;
    }
    let n = c / c.w;
    Some(egui::pos2(rect.min.x + (n.x + 1.0) * 0.5 * rect.width(), rect.min.y + (1.0 - n.y) * 0.5 * rect.height()))
}
