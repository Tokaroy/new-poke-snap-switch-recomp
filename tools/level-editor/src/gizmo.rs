//! Move and turn gizmo for the selected object, drawn with the egui painter over the 3D view, plus grid and angle
//! snapping. Three arrows move along X, Y and Z; the ring on the ground plane turns the heading (rotation about the up
//! axis, the same value as the inspector's Facing slider). Every gizmo drag is one undo step.

use crate::pick;
use crate::render::OrbitCamera;
use glam::Vec3;

/// Grid steps offered in the UI (world units; 0 = off).
pub const GRID_STEPS: [f32; 6] = [0.0, 0.25, 0.5, 1.0, 2.0, 5.0];
/// Angle steps offered in the UI (degrees; 0 = off).
pub const ANGLE_STEPS: [f32; 5] = [0.0, 5.0, 15.0, 45.0, 90.0];

const AXES: [Vec3; 3] = [Vec3::X, Vec3::Y, Vec3::Z];
const AXIS_COLORS: [egui::Color32; 3] =
    [egui::Color32::from_rgb(235, 80, 80), egui::Color32::from_rgb(110, 220, 90), egui::Color32::from_rgb(80, 140, 255)];
const RING_COLOR: egui::Color32 = egui::Color32::from_rgb(250, 210, 70);
const RING_SEGMENTS: usize = 48;
/// Pixels from a handle that still count as a hit.
const HIT_PX: f32 = 9.0;
/// Pixels around the center that belong to the object (ground drag), not to an arrow.
const CENTER_PX: f32 = 14.0;
/// Arrow length on screen, roughly.
const ARROW_PX: f32 = 90.0;

#[derive(Clone, Copy, Debug, PartialEq, Default)]
pub struct Snap {
    /// Grid step in world units; 0 = off.
    pub grid: f32,
    /// Angle step in degrees; 0 = off.
    pub angle: f32,
}

impl Snap {
    pub fn value(v: f32, step: f32) -> f32 {
        if step > 0.0 {
            (v / step).round() * step
        } else {
            v
        }
    }
    /// Snaps the horizontal position (X and Z) to the grid.
    pub fn xz(&self, p: Vec3) -> Vec3 {
        Vec3::new(Self::value(p.x, self.grid), p.y, Self::value(p.z, self.grid))
    }
    pub fn yaw(&self, yaw: f32) -> f32 {
        if self.angle > 0.0 {
            Self::value(yaw.to_degrees(), self.angle).to_radians()
        } else {
            yaw
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Handle {
    Axis(usize),
    Ring,
}

/// A gizmo drag in progress.
#[derive(Clone, Copy, Debug)]
pub struct GizmoDrag {
    pub handle: Handle,
    start_mouse: egui::Pos2,
    start_pos: Vec3,
    start_yaw: f32,
    start_angle: f32,
    /// Screen pixels per world unit along the dragged axis, and that axis' screen direction.
    px_per_unit: f32,
    screen_dir: egui::Vec2,
}

/// World length of the arrows so they keep about the same size on screen.
fn arrow_len(cam: &OrbitCamera, p: Vec3) -> f32 {
    (cam.eye().distance(p) * 0.12).max(0.2)
}

/// The ring sits outside the arrows so the two never cross on the ground plane.
fn ring_radius(cam: &OrbitCamera, p: Vec3) -> f32 {
    arrow_len(cam, p) * 1.3
}

fn ring_points(cam: &OrbitCamera, rect: egui::Rect, p: Vec3) -> Vec<egui::Pos2> {
    let r = ring_radius(cam, p);
    (0..=RING_SEGMENTS)
        .filter_map(|i| {
            let a = i as f32 / RING_SEGMENTS as f32 * std::f32::consts::TAU;
            pick::project(cam, rect, p + Vec3::new(a.sin() * r, 0.0, a.cos() * r))
        })
        .collect()
}

fn seg_dist(a: egui::Pos2, b: egui::Pos2, q: egui::Pos2) -> f32 {
    let ab = b - a;
    let t = ((q - a).dot(ab) / ab.length_sq().max(1e-6)).clamp(0.0, 1.0);
    (a + ab * t).distance(q)
}

/// Angle of the mouse around `p` on the horizontal plane through `p` (0 along +Z, same convention as the heading).
fn plane_angle(cam: &OrbitCamera, rect: egui::Rect, p: Vec3, at: egui::Pos2) -> Option<f32> {
    let (o, d) = pick::screen_ray(cam, rect, at);
    if d.y.abs() < 1e-4 {
        return None;
    }
    let t = (p.y - o.y) / d.y;
    if t <= 0.0 {
        return None;
    }
    let h = o + d * t - p;
    (h.x.abs() + h.z.abs() > 1e-5).then(|| h.x.atan2(h.z))
}

/// The handle under the mouse, if any. `ring` is false when the selection stores no heading.
pub fn hit(cam: &OrbitCamera, rect: egui::Rect, p: Vec3, ring: bool, at: egui::Pos2) -> Option<Handle> {
    let c = pick::project(cam, rect, p)?;
    if c.distance(at) < CENTER_PX {
        return None; // the middle stays a free drag along the ground
    }
    let len = arrow_len(cam, p);
    let mut best: Option<(f32, Handle)> = None;
    for (k, a) in AXES.iter().enumerate() {
        let Some(e) = pick::project(cam, rect, p + *a * len) else { continue };
        let d = seg_dist(c, e, at);
        if d < HIT_PX && best.map(|b| d < b.0).unwrap_or(true) {
            best = Some((d, Handle::Axis(k)));
        }
    }
    if ring {
        let pts = ring_points(cam, rect, p);
        for w in pts.windows(2) {
            let d = seg_dist(w[0], w[1], at);
            if d < HIT_PX && best.map(|b| d < b.0).unwrap_or(true) {
                best = Some((d, Handle::Ring));
            }
        }
    }
    best.map(|b| b.1)
}

/// Starts a drag on `handle` for an object at `p` with heading `yaw`.
pub fn begin(cam: &OrbitCamera, rect: egui::Rect, p: Vec3, yaw: f32, handle: Handle, at: egui::Pos2) -> Option<GizmoDrag> {
    let c = pick::project(cam, rect, p)?;
    let (px_per_unit, screen_dir) = match handle {
        Handle::Axis(k) => {
            let len = arrow_len(cam, p);
            let e = pick::project(cam, rect, p + AXES[k] * len)?;
            let v = e - c;
            if v.length() < 4.0 {
                return None; // axis points at the camera: no usable direction
            }
            (v.length() / len, v.normalized())
        }
        Handle::Ring => (1.0, egui::Vec2::ZERO),
    };
    let start_angle = if handle == Handle::Ring { plane_angle(cam, rect, p, at)? } else { 0.0 };
    Some(GizmoDrag { handle, start_mouse: at, start_pos: p, start_yaw: yaw, start_angle, px_per_unit, screen_dir })
}

pub enum DragResult {
    Move(Vec3),
    Yaw(f32),
}

impl GizmoDrag {
    /// The new position or heading for the mouse at `at`, snapped.
    pub fn update(&self, cam: &OrbitCamera, rect: egui::Rect, at: egui::Pos2, snap: Snap) -> Option<DragResult> {
        match self.handle {
            Handle::Axis(k) => {
                let px = (at - self.start_mouse).dot(self.screen_dir);
                let mut p = self.start_pos + AXES[k] * (px / self.px_per_unit.max(1e-4));
                p[k] = Snap::value(p[k], snap.grid);
                Some(DragResult::Move(p))
            }
            Handle::Ring => {
                let a = plane_angle(cam, rect, self.start_pos, at)?;
                let mut y = self.start_yaw + (a - self.start_angle);
                y = (y + std::f32::consts::PI).rem_euclid(std::f32::consts::TAU) - std::f32::consts::PI;
                Some(DragResult::Yaw(snap.yaw(y)))
            }
        }
    }
}

/// Draws the gizmo; `active` highlights the handle being dragged or hovered.
pub fn draw(painter: &egui::Painter, cam: &OrbitCamera, rect: egui::Rect, p: Vec3, ring: bool, active: Option<Handle>) {
    let Some(c) = pick::project(cam, rect, p) else { return };
    let len = arrow_len(cam, p);
    if ring {
        let pts = ring_points(cam, rect, p);
        let w = if active == Some(Handle::Ring) { 4.0 } else { 2.0 };
        painter.add(egui::Shape::line(pts, egui::Stroke::new(w, RING_COLOR)));
    }
    for (k, a) in AXES.iter().enumerate() {
        let Some(e) = pick::project(cam, rect, p + *a * len) else { continue };
        let v = e - c;
        if v.length() < 4.0 {
            continue;
        }
        let w = if active == Some(Handle::Axis(k)) { 5.0 } else { 3.0 };
        let col = AXIS_COLORS[k];
        painter.line_segment([c, e], egui::Stroke::new(w, col));
        // arrow head
        let u = v.normalized();
        let n = egui::vec2(-u.y, u.x);
        let head = (ARROW_PX * 0.14).min(v.length() * 0.35);
        painter.add(egui::Shape::convex_polygon(vec![e + u * head * 0.6, e - u * head * 0.6 + n * head * 0.5, e - u * head * 0.6 - n * head * 0.5], col, egui::Stroke::NONE));
        painter.text(e + u * head, egui::Align2::CENTER_CENTER, ["X", "Y", "Z"][k], egui::FontId::proportional(13.0), col);
    }
    painter.circle_filled(c, 4.0, egui::Color32::WHITE);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn snapping() {
        let s = Snap { grid: 0.5, angle: 15.0 };
        assert_eq!(s.xz(Vec3::new(1.26, 3.33, -0.74)), Vec3::new(1.5, 3.33, -0.5));
        assert!((s.yaw(20f32.to_radians()).to_degrees() - 15.0).abs() < 1e-3);
        let off = Snap::default();
        assert_eq!(off.xz(Vec3::new(1.26, 0.0, 2.0)), Vec3::new(1.26, 0.0, 2.0));
        assert_eq!(off.yaw(0.3), 0.3);
    }

    #[test]
    fn drags_follow_the_mouse() {
        let cam = OrbitCamera { target: Vec3::ZERO, yaw: 0.6, pitch: 0.8, dist: 30.0 };
        let rect = egui::Rect::from_min_size(egui::pos2(0.0, 0.0), egui::vec2(1600.0, 900.0));
        let p = Vec3::new(1.0, 2.0, 3.0);
        let c = pick::project(&cam, rect, p).unwrap();
        // X arrow: grab it halfway, move the mouse to where p + 2X projects; the object should land near there
        let len = arrow_len(&cam, p);
        let grab = pick::project(&cam, rect, p + Vec3::X * len * 0.5).unwrap();
        assert_eq!(hit(&cam, rect, p, true, grab), Some(Handle::Axis(0)));
        let g = begin(&cam, rect, p, 0.0, Handle::Axis(0), grab).unwrap();
        let to = grab + (pick::project(&cam, rect, p + Vec3::X * 2.0).unwrap() - c);
        let Some(DragResult::Move(q)) = g.update(&cam, rect, to, Snap::default()) else { panic!() };
        assert!((q - (p + Vec3::X * 2.0)).length() < 0.25, "{q:?}");
        assert_eq!((q.y, q.z), (p.y, p.z));
        let Some(DragResult::Move(q)) = g.update(&cam, rect, to, Snap { grid: 1.0, angle: 0.0 }) else { panic!() };
        assert_eq!(q.x, 3.0);
        // the middle is not a handle
        assert_eq!(hit(&cam, rect, p, true, c), None);
        // ring: a quarter turn of the mouse around the object turns it a quarter turn
        let r = ring_radius(&cam, p);
        let a0 = pick::project(&cam, rect, p + Vec3::new(0.0, 0.0, r)).unwrap();
        assert_eq!(hit(&cam, rect, p, true, a0), Some(Handle::Ring));
        let g = begin(&cam, rect, p, 0.1, Handle::Ring, a0).unwrap();
        let a1 = pick::project(&cam, rect, p + Vec3::new(r, 0.0, 0.0)).unwrap();
        let Some(DragResult::Yaw(y)) = g.update(&cam, rect, a1, Snap::default()) else { panic!() };
        assert!((y - (0.1 + std::f32::consts::FRAC_PI_2)).abs() < 0.02, "{y}");
        let Some(DragResult::Yaw(y)) = g.update(&cam, rect, a1, Snap { grid: 0.0, angle: 45.0 }) else { panic!() };
        assert!((y.to_degrees() - 90.0).abs() < 1e-3, "{}", y.to_degrees());
    }
}
