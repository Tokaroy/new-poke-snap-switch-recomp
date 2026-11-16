//! Putting copies on the surface: click to place one, a scatter brush that drops several around the mouse with a
//! random heading and size, and tilting scenery to the slope. Pure math over `Ground`, so the self-test can run it
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
}

impl Default for Brush {
    fn default() -> Self {
        Self { radius: 6.0, count: 3, spacing: 4.0, min_gap: 1.5, random_yaw: true, scale: (0.8, 1.2), align: false, seed: 1 }
    }
}

/// First surface straight below `p` (searching from `up` units above it) and its up-facing normal.
pub fn surface_below(g: &Ground, p: Vec3, up: f32) -> Option<(Vec3, Vec3)> {
    let h = g.raycast(p + Vec3::Y * up, -Vec3::Y, up + 500.0, None)?;
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

/// A single click: one copy on the surface point that was hit.
pub fn single(brush: &Brush, rng: &mut Rng, pos: Vec3, normal: Vec3) -> Spot {
    spot_at(brush, rng, pos, normal)
}

/// Spots for one scatter dab around `center`, skipping places closer than `min_gap` to `taken` (which grows).
pub fn dab(g: &Ground, brush: &Brush, rng: &mut Rng, center: Vec3, taken: &mut Vec<Vec3>) -> Vec<Spot> {
    let mut out = vec![];
    for _ in 0..brush.count {
        for _try in 0..8 {
            // uniform over the disc
            let r = brush.radius * rng.unit().sqrt();
            let a = rng.range(0.0, std::f32::consts::TAU);
            let q = center + Vec3::new(a.sin() * r, 0.0, a.cos() * r);
            let Some((p, n)) = surface_below(g, q, brush.radius + 10.0) else { continue };
            if taken.iter().any(|t| Vec3::new(t.x - p.x, 0.0, t.z - p.z).length() < brush.min_gap) {
                continue;
            }
            taken.push(p);
            out.push(spot_at(brush, rng, p, n));
            break;
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pick::GroundModel;

    /// A 40x40 ramp rising 1 unit per 2 along +X (about 26.6 degrees).
    fn ramp() -> Ground {
        let pos = vec![Vec3::new(-20.0, -10.0, -20.0), Vec3::new(20.0, 10.0, -20.0), Vec3::new(20.0, 10.0, 20.0), Vec3::new(-20.0, -10.0, 20.0)];
        Ground {
            pos,
            idx: vec![0, 1, 2, 0, 2, 3],
            models: vec![GroundModel { model: 0, tri_start: 0, tri_end: 2, min: Vec3::new(-20.0, -10.0, -20.0), max: Vec3::new(20.0, 10.0, 20.0) }],
        }
    }

    #[test]
    fn scatter_lands_on_the_ground_apart_and_repeats() {
        let g = ramp();
        let b = Brush { radius: 8.0, count: 12, min_gap: 2.0, ..Default::default() };
        let run = || {
            let mut taken = vec![];
            dab(&g, &b, &mut Rng::new(42), Vec3::ZERO, &mut taken)
        };
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
        let g = ramp();
        let (_, n) = surface_below(&g, Vec3::new(3.0, 0.0, 1.0), 20.0).unwrap();
        let want = Vec3::new(-0.5, 1.0, 0.0).normalize();
        assert!(n.dot(want) > 0.9999, "{n:?}");
        let q = orient([0.0, 0.0, 0.0, 1.0], Some(0.7), n, true);
        assert!((q * Vec3::Y).dot(n) > 0.9999);
        let q = orient([0.0, 0.0, 0.0, 1.0], Some(0.7), n, false);
        assert!((q * Vec3::Y).dot(Vec3::Y) > 0.9999);
        assert!((crate::edit::yaw_of(q.to_array()) - 0.7).abs() < 1e-4);
    }
}
