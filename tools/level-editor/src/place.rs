//! Putting copies on the surface: click to place one, a scatter brush that drops several around the mouse with a
//! random heading and size, tilting scenery to the slope, and rule-based scatter (slope, height, distance from the
//! rail, ground only) for both the brush and "fill the course". Pure math over `Ground`, so the self-test can run it
//! without a window. The same seed gives the same layout.

use crate::pick::Ground;
use glam::{EulerRot, Quat, Vec3};

/// Small deterministic random source (SplitMix64); no extra crate needed.
#[derive(Clone, Debug)]
pub struct Rng(u64);

impl Rng {
    pub fn new(seed: u64) -> Self {
        Self(seed ^ 0x9E37_79B9_7F4A_7C15)
    }
    pub fn next_u64(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }
    /// Uniform in [0, 1).
    pub fn unit(&mut self) -> f32 {
        (self.next_u64() >> 40) as f32 / (1u64 << 24) as f32
    }
    pub fn range(&mut self, a: f32, b: f32) -> f32 {
        a + (b - a) * self.unit()
    }
}

/// Where copies may land. Every rule is off by default.
#[derive(Clone, Copy, Debug, PartialEq, Default)]
pub struct Rules {
    /// Steepest ground allowed, in degrees from flat.
    pub max_slope: Option<f32>,
    /// World height (Y) range.
    pub height: Option<(f32, f32)>,
    /// Horizontal distance range from the camera rail.
    pub rail_dist: Option<(f32, f32)>,
    /// Land on the course's ground models only, not on trees, rocks or water.
    pub ground_only: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SurfaceKind {
    Ground,
    Other,
    /// Sky domes and backdrops: never a surface.
    Skip,
}

/// What the rules need to know about the course.
#[derive(Clone, Debug, Default)]
pub struct Site {
    /// Camera rail parts as polylines.
    pub rail: Vec<Vec<Vec3>>,
    /// Surface kind by model index; models past the end (fresh copies) count as Other.
    pub kinds: Vec<SurfaceKind>,
}

impl Site {
    pub fn of(c: &crate::course::LoadedCourse) -> Self {
        Site {
            rail: c.rail().map(|r| r.segments.iter().map(|s| s.iter().map(|p| Vec3::from(*p)).collect()).collect()).unwrap_or_default(),
            kinds: c.models.iter().map(|m| Self::kind_of_name(&m.inst.name)).collect(),
        }
    }
    /// Classifies a model by its instance name (SM_ENV_*_Ground*, *_Sky*, *_Backdrop*).
    pub fn kind_of_name(name: &str) -> SurfaceKind {
        let n = name.to_ascii_lowercase();
        if n.contains("sky") || n.contains("backdrop") {
            SurfaceKind::Skip
        } else if n.contains("ground") {
            SurfaceKind::Ground
        } else {
            SurfaceKind::Other
        }
    }
    fn kind(&self, m: usize) -> SurfaceKind {
        self.kinds.get(m).copied().unwrap_or(SurfaceKind::Other)
    }
    /// Horizontal distance from `p` to the rail (infinite without a rail).
    pub fn rail_distance(&self, p: Vec3) -> f32 {
        let flat = |v: Vec3| glam::Vec2::new(v.x, v.z);
        let q = flat(p);
        let mut best = f32::INFINITY;
        for part in &self.rail {
            for w in part.windows(2) {
                let (a, b) = (flat(w[0]), flat(w[1]));
                let ab = b - a;
                let t = ((q - a).dot(ab) / ab.length_squared().max(1e-9)).clamp(0.0, 1.0);
                best = best.min((a + ab * t).distance(q));
            }
        }
        best
    }
    /// Horizontal bounds of the rail, widened by `margin`.
    pub fn rail_bounds(&self, margin: f32) -> Option<(Vec3, Vec3)> {
        let pts: Vec<Vec3> = self.rail.iter().flatten().copied().collect();
        if pts.is_empty() {
            return None;
        }
        let (mn, mx) = pts.iter().fold((Vec3::splat(f32::MAX), Vec3::splat(f32::MIN)), |(a, b), p| (a.min(*p), b.max(*p)));
        Some((mn - Vec3::splat(margin), mx + Vec3::splat(margin)))
    }
}

impl Rules {
    pub fn allows(&self, site: &Site, pos: Vec3, normal: Vec3) -> bool {
        if let Some(max) = self.max_slope {
            if normal.normalize_or_zero().y < max.to_radians().cos() - 1e-6 {
                return false;
            }
        }
        if let Some((lo, hi)) = self.height {
            if pos.y < lo || pos.y > hi {
                return false;
            }
        }
        if let Some((lo, hi)) = self.rail_dist {
            let d = site.rail_distance(pos);
            if d < lo || d > hi {
                return false;
            }
        }
        true
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Brush {
    /// Scatter circle radius, world units.
    pub radius: f32,
    /// Copies dropped at each dab.
    pub count: usize,
    /// How far the mouse moves (world units) before the next dab.
    pub spacing: f32,
    /// Copies from one stroke keep at least this far apart (horizontal distance).
    pub min_gap: f32,
    /// A random heading for each copy; off keeps the source's heading.
    pub random_yaw: bool,
    /// Random size range for scenery models (Pokémon spawns store no size). 1..1 keeps the source size.
    pub scale: (f32, f32),
    /// Tilt scenery so its up axis follows the ground. Pokémon always stay upright.
    pub align: bool,
    pub seed: u64,
    pub rules: Rules,
}

impl Default for Brush {
    fn default() -> Self {
        Self { radius: 6.0, count: 3, spacing: 4.0, min_gap: 1.5, random_yaw: true, scale: (0.8, 1.2), align: false, seed: 1, rules: Rules::default() }
    }
}

/// First surface straight below `p` (searching from `up` units above it) and its up-facing normal. Sky and backdrop
/// models are never a surface; with `ground_only` only ground models are.
pub fn surface_below(g: &Ground, site: &Site, p: Vec3, up: f32, ground_only: bool) -> Option<(Vec3, Vec3)> {
    let keep = |m: usize| match site.kind(m) {
        SurfaceKind::Skip => false,
        SurfaceKind::Ground => true,
        SurfaceKind::Other => !ground_only,
    };
    let h = g.raycast_where(p + Vec3::Y * up, -Vec3::Y, up + 500.0, &keep)?;
    let n = if h.normal.y < 0.0 { -h.normal } else { h.normal };
    Some((h.point, n))
}

/// Rotation for a copy. `yaw` replaces the heading (None keeps the source's); with `align` the copy's up axis follows
/// `normal`, otherwise the source's own tilt is kept.
pub fn orient(src_rot: [f32; 4], yaw: Option<f32>, normal: Vec3, align: bool) -> Quat {
    let src = Quat::from_array(src_rot);
    let src = if src.length_squared() < 1e-6 { Quat::IDENTITY } else { src.normalize() };
    let (sy, sx, sz) = src.to_euler(EulerRot::YXZ);
    let y = yaw.unwrap_or(sy);
    if align && normal.length_squared() > 1e-6 {
        Quat::from_rotation_arc(Vec3::Y, normal.normalize()) * Quat::from_rotation_y(y)
    } else {
        Quat::from_euler(EulerRot::YXZ, y, sx, sz)
    }
}

/// One copy to make.
#[derive(Clone, Copy, Debug)]
pub struct Spot {
    pub pos: Vec3,
    pub normal: Vec3,
    /// Heading for the copy, when the brush randomizes it.
    pub yaw: Option<f32>,
    pub scale_mul: f32,
}

fn spot_at(brush: &Brush, rng: &mut Rng, pos: Vec3, normal: Vec3) -> Spot {
    let yaw = brush.random_yaw.then(|| rng.range(-std::f32::consts::PI, std::f32::consts::PI));
    let (a, b) = (brush.scale.0.min(brush.scale.1), brush.scale.0.max(brush.scale.1));
    let scale_mul = if (b - a).abs() < 1e-6 { a } else { rng.range(a, b) };
    Spot { pos, normal, yaw, scale_mul }
}

/// A single click: one copy on the surface point that was hit (rules do not apply to a deliberate click).
pub fn single(brush: &Brush, rng: &mut Rng, pos: Vec3, normal: Vec3) -> Spot {
    spot_at(brush, rng, pos, normal)
}

fn far_from(taken: &[Vec3], p: Vec3, gap: f32) -> bool {
    !taken.iter().any(|t| Vec3::new(t.x - p.x, 0.0, t.z - p.z).length() < gap)
}

/// Spots for one scatter dab around `center` that pass the brush's rules, skipping places closer than `min_gap` to
/// `taken` (which grows).
pub fn dab(g: &Ground, site: &Site, brush: &Brush, rng: &mut Rng, center: Vec3, taken: &mut Vec<Vec3>) -> Vec<Spot> {
    let mut out = vec![];
    for _ in 0..brush.count {
        for _try in 0..8 {
            // uniform over the disc
            let r = brush.radius * rng.unit().sqrt();
            let a = rng.range(0.0, std::f32::consts::TAU);
            let q = center + Vec3::new(a.sin() * r, 0.0, a.cos() * r);
            let Some((p, n)) = surface_below(g, site, q, brush.radius + 10.0, brush.rules.ground_only) else { continue };
            if !far_from(taken, p, brush.min_gap) || !brush.rules.allows(site, p, n) {
                continue;
            }
            taken.push(p);
            out.push(spot_at(brush, rng, p, n));
            break;
        }
    }
    out
}

/// Rule-based fill: up to `n` spots spread over the horizontal box `min`..`max` that pass the brush's rules and keep
/// the gap from each other and from `taken`. Gives up after 40 tries per copy (tight rules can find fewer).
pub fn fill(g: &Ground, site: &Site, brush: &Brush, rng: &mut Rng, min: Vec3, max: Vec3, n: usize, taken: &mut Vec<Vec3>) -> Vec<Spot> {
    let mut out = vec![];
    let top = max.y + 50.0;
    for _ in 0..n.saturating_mul(40) {
        if out.len() >= n {
            break;
        }
        let q = Vec3::new(rng.range(min.x, max.x), top, rng.range(min.z, max.z));
        let Some((p, nrm)) = surface_below(g, site, q, 0.0, brush.rules.ground_only) else { continue };
        if !far_from(taken, p, brush.min_gap) || !brush.rules.allows(site, p, nrm) {
            continue;
        }
        taken.push(p);
        out.push(spot_at(brush, rng, p, nrm));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pick::GroundModel;

    /// A 40x40 ramp rising 1 unit per 2 along +X (about 26.6 degrees) as model 0, plus a flat "sky" lid at y = 30 as
    /// model 1.
    fn ramp() -> (Ground, Site) {
        let pos = vec![
            Vec3::new(-20.0, -10.0, -20.0),
            Vec3::new(20.0, 10.0, -20.0),
            Vec3::new(20.0, 10.0, 20.0),
            Vec3::new(-20.0, -10.0, 20.0),
            Vec3::new(-50.0, 30.0, -50.0),
            Vec3::new(50.0, 30.0, -50.0),
            Vec3::new(50.0, 30.0, 50.0),
            Vec3::new(-50.0, 30.0, 50.0),
        ];
        let g = Ground {
            pos,
            idx: vec![0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7],
            models: vec![
                GroundModel { model: 0, tri_start: 0, tri_end: 2, min: Vec3::new(-20.0, -10.0, -20.0), max: Vec3::new(20.0, 10.0, 20.0) },
                GroundModel { model: 1, tri_start: 2, tri_end: 4, min: Vec3::new(-50.0, 30.0, -50.0), max: Vec3::new(50.0, 30.0, 50.0) },
            ],
        };
        let site = Site {
            rail: vec![vec![Vec3::new(0.0, 0.0, -20.0), Vec3::new(0.0, 0.0, 20.0)]],
            kinds: vec![Site::kind_of_name("SM_ENV_X_Ground"), Site::kind_of_name("SM_ENV_X_Sky")],
        };
        (g, site)
    }

    #[test]
    fn scatter_lands_on_the_ground_apart_and_repeats() {
        let (g, site) = ramp();
        let b = Brush { radius: 8.0, count: 12, min_gap: 2.0, ..Default::default() };
        let run = || dab(&g, &site, &b, &mut Rng::new(42), Vec3::ZERO, &mut vec![]);
        let s = run();
        assert!(s.len() >= 8, "{}", s.len());
        for (i, a) in s.iter().enumerate() {
            assert!((a.pos.y - a.pos.x * 0.5).abs() < 1e-3, "on the ramp: {:?}", a.pos);
            assert!(Vec3::new(a.pos.x, 0.0, a.pos.z).length() <= 8.0 + 1e-3);
            assert!(a.scale_mul >= 0.8 && a.scale_mul <= 1.2);
            for b2 in &s[i + 1..] {
                assert!(Vec3::new(a.pos.x - b2.pos.x, 0.0, a.pos.z - b2.pos.z).length() >= 2.0);
            }
        }
        let again = run();
        assert!(s.iter().zip(&again).all(|(a, b)| a.pos == b.pos && a.yaw == b.yaw));
    }

    #[test]
    fn align_follows_the_slope() {
        let (g, site) = ramp();
        let (_, n) = surface_below(&g, &site, Vec3::new(3.0, 0.0, 1.0), 20.0, false).unwrap();
        let want = Vec3::new(-0.5, 1.0, 0.0).normalize();
        assert!(n.dot(want) > 0.9999, "{n:?}");
        let q = orient([0.0, 0.0, 0.0, 1.0], Some(0.7), n, true);
        assert!((q * Vec3::Y).dot(n) > 0.9999);
        let q = orient([0.0, 0.0, 0.0, 1.0], Some(0.7), n, false);
        assert!((q * Vec3::Y).dot(Vec3::Y) > 0.9999);
        assert!((crate::edit::yaw_of(q.to_array()) - 0.7).abs() < 1e-4);
    }

    #[test]
    fn rules_filter_the_fill() {
        let (g, site) = ramp();
        let (mn, mx) = (Vec3::new(-20.0, -10.0, -20.0), Vec3::new(20.0, 10.0, 20.0));
        // the sky lid is never a surface, so the fill reaches the ramp from above
        let b = Brush { min_gap: 1.0, ..Default::default() };
        let all = fill(&g, &site, &b, &mut Rng::new(3), mn, mx, 30, &mut vec![]);
        assert_eq!(all.len(), 30);
        assert!(all.iter().all(|s| s.pos.y < 11.0));
        // the ramp is 26.6 degrees: a 20 degree limit rejects all of it, 30 accepts it
        let steep = Brush { rules: Rules { max_slope: Some(20.0), ..Default::default() }, ..b };
        assert!(fill(&g, &site, &steep, &mut Rng::new(3), mn, mx, 10, &mut vec![]).is_empty());
        let ok = Brush { rules: Rules { max_slope: Some(30.0), ..Default::default() }, ..b };
        assert_eq!(fill(&g, &site, &ok, &mut Rng::new(3), mn, mx, 10, &mut vec![]).len(), 10);
        // height 0..5 and 4..8 units from the rail (the line x = 0): x between 4 and 8 on the upper half
        let band = Brush { rules: Rules { height: Some((0.0, 5.0)), rail_dist: Some((4.0, 8.0)), ..Default::default() }, ..b };
        let s = fill(&g, &site, &band, &mut Rng::new(5), mn, mx, 20, &mut vec![]);
        assert!(s.len() >= 10, "{}", s.len());
        assert!(s.iter().all(|s| s.pos.x >= 4.0 - 1e-3 && s.pos.x <= 8.0 + 1e-3 && s.pos.y >= 0.0 && s.pos.y <= 5.0), "{s:?}");
    }
}
