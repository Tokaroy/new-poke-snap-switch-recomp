//! Random course (owner list item 7). The separate `snap-procgen` tool (GPL-3.0, so it runs as its own program and only
//! its JSON is read here) makes a course from a seed: heightfield, texture splat, rail, scenery and spawn points, in
//! feet. This module lays that course over the first rail part of an existing course leg:
//!
//! - the rail keeps its point count (the game ties event records to each edge) and its ends, so the ride still joins
//!   the rest of the leg; in between it swings sideways like the generated rail and rides up and down with the new
//!   ground;
//! - the ground in a band along the rail takes the generated relief, scaled so the generated rail length fits the
//!   leg's rail, fading out at the band's edge and near the rail's ends;
//! - the texture layer follows the generated splat (grass or bare soil), with a dirt path under the new rail;
//! - scenery and Pokémon spawn points keep their place relative to the generated rail.
//!
//! Everything here is a plan of grid cells and positions; `EditorApp::generate_course` writes it through the undo
//! history, so one undo removes the whole course.

use crate::ground::Terrain;
use glam::{Vec2, Vec3};
use serde_json::Value;
use std::path::Path;

const Q: f32 = 1024.0;

/// A course from snap-procgen's JSON (format version 2), in feet.
pub struct Gen {
    pub seed: u64,
    pub biome: String,
    res: usize,
    width: f32,
    depth: f32,
    heights: Vec<f32>,
    splat: Vec<[u8; 8]>,
    /// rail samples: position and distance along the rail
    rail: Vec<Vec3>,
    dist: Vec<f32>,
    pub props: Vec<Prop>,
    pub spawns: Vec<GenSpawn>,
}

pub struct Prop {
    pub class: String,
    pub pos: Vec3,
    /// radians
    pub yaw: f32,
    pub scale: f32,
}

pub struct GenSpawn {
    pub habitat: String,
    pub pos: Vec3,
}

fn num(v: &Value) -> Result<f64, String> {
    v.as_f64().ok_or_else(|| format!("number expected, found {v}"))
}
fn vec3q(v: &Value) -> Result<Vec3, String> {
    let a = v.as_array().filter(|a| a.len() == 3).ok_or("a position of three numbers expected")?;
    Ok(Vec3::new(num(&a[0])? as f32, num(&a[1])? as f32, num(&a[2])? as f32) / Q)
}
fn arr<'a>(v: &'a Value, key: &str) -> Result<&'a Vec<Value>, String> {
    v.get(key).and_then(Value::as_array).ok_or_else(|| format!("course file: no list \"{key}\""))
}

impl Gen {
    pub fn load(path: &Path) -> Result<Gen, String> {
        let text = std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
        Self::parse(&text)
    }

    pub fn parse(text: &str) -> Result<Gen, String> {
        let v: Value = serde_json::from_str(text).map_err(|e| format!("course file is not JSON: {e}"))?;
        let ver = v.get("format_version").and_then(Value::as_u64).unwrap_or(0);
        if ver != 2 {
            return Err(format!("course file format {ver}: this editor reads snap-procgen format 2"));
        }
        let t = v.get("terrain").ok_or("course file: no terrain")?;
        let res = t.get("resolution").and_then(Value::as_u64).ok_or("course file: no terrain resolution")? as usize;
        let width = num(t.get("width_q").ok_or("no width_q")?)? as f32 / Q;
        let depth = num(t.get("depth_q").ok_or("no depth_q")?)? as f32 / Q;
        let heights = arr(t, "heights_q")?.iter().map(|h| num(h).map(|h| h as f32 / Q)).collect::<Result<Vec<_>, _>>()?;
        let splat = arr(t, "splat")?
            .iter()
            .map(|s| {
                let a = s.as_array().filter(|a| a.len() == 8).ok_or("splat entries have 8 channels")?;
                let mut o = [0u8; 8];
                for (k, c) in a.iter().enumerate() {
                    o[k] = c.as_u64().unwrap_or(0).min(255) as u8;
                }
                Ok(o)
            })
            .collect::<Result<Vec<_>, String>>()?;
        if res < 2 || heights.len() != res * res || splat.len() != res * res {
            return Err("course file: terrain arrays do not match its resolution".into());
        }
        let samples = arr(v.get("rail").ok_or("course file: no rail")?, "samples")?;
        let mut rail = vec![];
        let mut dist = vec![];
        for s in samples {
            rail.push(vec3q(s.get("position_q").ok_or("rail sample without position")?)?);
            dist.push(num(s.get("distance_q").ok_or("rail sample without distance")?)? as f32 / Q);
        }
        if rail.len() < 2 {
            return Err("course file: the rail has fewer than two samples".into());
        }
        let props = arr(&v, "scenery")?
            .iter()
            .map(|p| {
                Ok(Prop {
                    class: p.get("class").and_then(Value::as_str).unwrap_or("").to_string(),
                    pos: vec3q(p.get("position_q").ok_or("scenery without position")?)?,
                    yaw: (p.get("yaw_millidegrees").and_then(Value::as_f64).unwrap_or(0.0) / 1000.0).to_radians() as f32,
                    scale: p.get("scale_permille").and_then(Value::as_f64).unwrap_or(1000.0) as f32 / 1000.0,
                })
            })
            .collect::<Result<Vec<_>, String>>()?;
        let spawns = arr(&v, "spawns")?
            .iter()
            .map(|p| {
                Ok(GenSpawn {
                    habitat: p.get("habitat").and_then(Value::as_str).unwrap_or("").to_string(),
                    pos: vec3q(p.get("position_q").ok_or("spawn without position")?)?,
                })
            })
            .collect::<Result<Vec<_>, String>>()?;
        let params = v.get("params");
        Ok(Gen {
            seed: v.get("seed").and_then(Value::as_u64).unwrap_or(0),
            biome: params.and_then(|p| p.get("biome")).and_then(Value::as_str).unwrap_or("").to_string(),
            res,
            width,
            depth,
            heights,
            splat,
            rail,
            dist,
            props,
            spawns,
        })
    }

    pub fn rail_length(&self) -> f32 {
        *self.dist.last().unwrap()
    }

    fn grid(&self, x: f32, z: f32) -> (usize, usize, f32, f32) {
        let n = self.res - 1;
        let gx = (x / self.width * n as f32).clamp(0.0, n as f32);
        let gz = (z / self.depth * n as f32).clamp(0.0, n as f32);
        let (ix, iz) = ((gx as usize).min(n - 1), (gz as usize).min(n - 1));
        (ix, iz, gx - ix as f32, gz - iz as f32)
    }

    /// Ground height in feet at (x, z) feet.
    pub fn height(&self, x: f32, z: f32) -> f32 {
        let (ix, iz, tx, tz) = self.grid(x, z);
        let h = |a: usize, b: usize| self.heights[(iz + b) * self.res + ix + a];
        (h(0, 0) * (1.0 - tx) + h(1, 0) * tx) * (1.0 - tz) + (h(0, 1) * (1.0 - tx) + h(1, 1) * tx) * tz
    }

    /// Share of grass and moss in the splat at (x, z), 0 to 1 (the rest is sand, soil, rock and the like).
    pub fn green(&self, x: f32, z: f32) -> f32 {
        let (ix, iz, tx, tz) = self.grid(x, z);
        let g = |a: usize, b: usize| {
            let s = self.splat[(iz + b) * self.res + ix + a];
            let tot: f32 = s.iter().map(|&c| c as f32).sum();
            if tot <= 0.0 {
                0.0
            } else {
                (s[2] as f32 + s[7] as f32) / tot
            }
        };
        (g(0, 0) * (1.0 - tx) + g(1, 0) * tx) * (1.0 - tz) + (g(0, 1) * (1.0 - tx) + g(1, 1) * tx) * tz
    }

    fn rail_xz(&self) -> Vec<Vec2> {
        self.rail.iter().map(|p| Vec2::new(p.x, p.z)).collect()
    }
}

/// A rail as a chain of points in the ground plane, with lengths, for "how far along, how far to the side" questions.
pub struct Spine {
    pts: Vec<Vec2>,
    cum: Vec<f32>,
}

/// Left of travel (glam's `perp`). Generated and target rails use the same rule, so sides carry over.
fn side(t: Vec2) -> Vec2 {
    t.perp()
}

impl Spine {
    pub fn new(pts: Vec<Vec2>) -> Spine {
        let mut cum = vec![0.0];
        for w in pts.windows(2) {
            cum.push(cum.last().unwrap() + w[0].distance(w[1]));
        }
        Spine { pts, cum }
    }
    pub fn length(&self) -> f32 {
        *self.cum.last().unwrap()
    }
    /// (fraction along the rail 0..1, signed distance to the left) of the nearest point on the chain.
    pub fn project(&self, p: Vec2) -> (f32, f32) {
        let mut best = (f32::MAX, 0.0, 0.0);
        for i in 0..self.pts.len() - 1 {
            let (a, b) = (self.pts[i], self.pts[i + 1]);
            let ab = b - a;
            let l2 = ab.length_squared();
            let t = if l2 < 1e-9 { 0.0 } else { ((p - a).dot(ab) / l2).clamp(0.0, 1.0) };
            let q = a + ab * t;
            let d2 = p.distance_squared(q);
            if d2 < best.0 {
                let s = side(ab.normalize_or_zero()).dot(p - q);
                let along = self.cum[i] + t * l2.sqrt();
                best = (d2, along, if s < 0.0 { -d2.sqrt() } else { d2.sqrt() });
            }
        }
        (best.1 / self.length().max(1e-6), best.2)
    }
    /// Point and unit direction at fraction `u` along the chain.
    pub fn at(&self, u: f32) -> (Vec2, Vec2) {
        let d = u.clamp(0.0, 1.0) * self.length();
        let i = self.cum.partition_point(|&c| c <= d).clamp(1, self.pts.len() - 1) - 1;
        let seg = self.cum[i + 1] - self.cum[i];
        let t = if seg < 1e-6 { 0.0 } else { (d - self.cum[i]) / seg };
        let (a, b) = (self.pts[i], self.pts[i + 1]);
        (a.lerp(b, t), (b - a).normalize_or_zero())
    }
    /// Fraction along the chain of point `i`.
    pub fn u_of(&self, i: usize) -> f32 {
        self.cum[i] / self.length().max(1e-6)
    }
}

fn smooth01(t: f32) -> f32 {
    let t = t.clamp(0.0, 1.0);
    t * t * (3.0 - 2.0 * t)
}

/// How strongly the generated course applies at fraction `u` along the rail: none at the ends, so the ride still meets
/// the rest of the leg.
pub fn envelope(u: f32) -> f32 {
    smooth01(u / 0.12) * smooth01((1.0 - u) / 0.12)
}

#[derive(Clone, Copy)]
pub struct Opts {
    /// width of the reshaped band on each side of the rail, in course units
    pub band: f32,
    /// largest sideways move of the rail
    pub swing: f32,
    /// largest height change of the ground
    pub relief: f32,
    /// width of the dirt path on each side of the rail
    pub path: f32,
    /// room the swung rail keeps from the course's own scenery and slopes
    pub clearance: f32,
    pub max_props: usize,
    pub max_spawns: usize,
}

impl Default for Opts {
    fn default() -> Self {
        Opts { band: 10.0, swing: 3.5, relief: 4.0, path: 2.2, clearance: 1.6, max_props: 60, max_spawns: 12 }
    }
}

pub struct PlacedProp {
    pub class: String,
    /// position in the ground plane (y = 0; the caller drops it to the ground)
    pub pos: Vec2,
    pub yaw: f32,
    pub scale: f32,
}

pub struct PlacedSpawn {
    pub air: bool,
    pub pos: Vec2,
    /// heading toward the rail, radians
    pub yaw: f32,
}

pub struct Plan {
    /// course units per foot
    pub scale: f32,
    /// new position of each point of rail part 0
    pub rail: Vec<Vec3>,
    /// (cell, new height change) and (cell, new layer blend)
    pub height: Vec<(usize, f32)>,
    pub layer: Vec<(usize, f32)>,
    pub props: Vec<PlacedProp>,
    pub spawns: Vec<PlacedSpawn>,
    /// height change of the rail: lowest, highest
    pub rail_relief: (f32, f32),
    pub swing: f32,
}

/// Something in the way: distance along a ray (origin, unit direction, longest distance) to the first surface.
pub type Clear<'a> = &'a dyn Fn(Vec3, Vec3, f32) -> Option<f32>;

/// Lays `g` over a rail part whose points are `rail` on terrain `t`. `clear` finds the course's own geometry, so the
/// swung rail stays out of it.
pub fn plan(g: &Gen, rail: &[Vec3], t: &Terrain, o: Opts, clear: Clear) -> Result<Plan, String> {
    if rail.len() < 4 {
        return Err("the rail part is too short to carry a course".into());
    }
    let old = Spine::new(rail.iter().map(|p| Vec2::new(p.x, p.z)).collect());
    let gen = Spine::new(g.rail_xz());
    let s = old.length() / gen.length().max(1e-6);
    // generated rail's sideways wander against the straight line from its start to its end
    let (g0, g1) = (gen.pts[0], *gen.pts.last().unwrap());
    let chord = (g1 - g0).normalize_or_zero();
    let wander = |u: f32| -> f32 {
        let (p, _) = gen.at(u);
        side(chord).dot(p - g0)
    };
    // the mean generated height under the rail is the leg's "zero"
    let n = 64;
    let mean_h = (0..=n).map(|k| gen.at(k as f32 / n as f32).0).map(|p| g.height(p.x, p.y)).sum::<f32>() / (n + 1) as f32;
    let relief = |h_ft: f32| o.relief * ((h_ft - mean_h) * s / o.relief).tanh();

    // 1. sideways swing of the rail (ends stay). The swing stops short of the course's own scenery and slopes: a ray at
    // ride height toward the new spot must stay clear for the swing plus `o.clearance`.
    let mut want = vec![];
    let mut room = vec![];
    for i in 0..rail.len() {
        let u = old.u_of(i);
        let (_, dir) = old.at(u);
        let raw = wander(u) * s;
        let v = o.swing * (raw / o.swing).tanh() * envelope(u);
        let n = side(dir) * v.signum();
        let n3 = Vec3::new(n.x, 0.0, n.y);
        let mut r = v.abs();
        // a fan of rays at three heights, so thin leaves and branches are found too
        for h in [0.4, 1.2, 2.0] {
            for a in [-0.6f32, -0.3, 0.0, 0.3, 0.6] {
                let d3 = glam::Quat::from_rotation_y(a) * n3;
                if let Some(d) = clear(rail[i] + Vec3::Y * h, d3, (v.abs() + o.clearance) / a.cos()) {
                    r = r.min((d * a.cos() - o.clearance).max(0.0));
                }
            }
        }
        want.push(v);
        room.push(r);
    }
    // a blocked point holds its neighbors back too, so the rail bends around instead of kinking
    let mut v: Vec<f32> = (0..rail.len())
        .map(|i| {
            let lo = i.saturating_sub(2);
            let hi = (i + 2).min(rail.len() - 1);
            let r = room[lo..=hi].iter().cloned().fold(f32::MAX, f32::min);
            want[i].signum() * want[i].abs().min(r)
        })
        .collect();
    for _ in 0..2 {
        let prev = v.clone();
        for i in 1..v.len() - 1 {
            let m = 0.25 * prev[i - 1] + 0.5 * prev[i] + 0.25 * prev[i + 1];
            // smoothing never pushes a point further out than its room
            v[i] = if m * want[i] < 0.0 { 0.0 } else { m.signum() * m.abs().min(room[i]) };
        }
    }
    let mut xz: Vec<Vec2> = vec![];
    let mut swing_max: f32 = 0.0;
    for i in 0..rail.len() {
        let (_, dir) = old.at(old.u_of(i));
        swing_max = swing_max.max(v[i].abs());
        xz.push(Vec2::new(rail[i].x, rail[i].z) + side(dir) * v[i]);
    }
    let new = Spine::new(xz.clone());

    // 2. ground: generated relief in a band along the new rail
    let mut delta = t.delta.clone();
    let mut layer = t.layer.clone();
    let mut height = vec![];
    let mut paint = vec![];
    for i in 0..t.base.len() {
        if t.base[i].is_nan() {
            continue;
        }
        let (x, z) = t.point(i);
        let (u, d) = new.project(Vec2::new(x, z));
        let env = envelope(u);
        if d.abs() > o.band || env <= 0.0 {
            continue;
        }
        let fall = smooth01((o.band - d.abs()) / (o.band * 0.4)) * env;
        let (gp, gdir) = gen.at(u);
        let gq = gp + side(gdir) * (d / s);
        let h = relief(g.height(gq.x, gq.y));
        let nd = t.delta[i] + h * fall;
        delta[i] = nd;
        height.push((i, nd));
        // texture: dirt path under the rail, elsewhere grass or bare soil from the splat
        let target = if d.abs() < o.path {
            -1.0
        } else if g.green(gq.x, gq.y) >= 0.35 {
            1.0
        } else {
            -1.0
        };
        let w = if d.abs() < o.path { env } else { fall };
        let nl = (t.layer[i] + target * w).clamp(-1.0, 1.0);
        layer[i] = nl;
        paint.push((i, nl));
    }

    // 3. rail height: follows the new ground, keeps its clearance; lightly smoothed
    let mut ys: Vec<f32> = vec![];
    let mut lo = f32::MAX;
    let mut hi = f32::MIN;
    for i in 0..rail.len() {
        let edit = t.sample(&delta, xz[i].x, xz[i].y) - t.sample(&t.delta, rail[i].x, rail[i].z);
        // a sideways move onto a bank lifts the rail with it (up to 3 units); into a dip it drops at most 1
        let bank = t.base_at(xz[i].x, xz[i].y) - t.base_at(rail[i].x, rail[i].z);
        let bank = if bank.is_nan() { 0.0 } else { bank.clamp(-1.0, 3.0) };
        let dy = edit + bank;
        lo = lo.min(dy);
        hi = hi.max(dy);
        ys.push(rail[i].y + dy);
    }
    for _ in 0..2 {
        let prev = ys.clone();
        for i in 1..ys.len() - 1 {
            ys[i] = 0.25 * prev[i - 1] + 0.5 * prev[i] + 0.25 * prev[i + 1];
        }
    }
    let new_rail: Vec<Vec3> = (0..rail.len()).map(|i| if i == 0 || i == rail.len() - 1 { rail[i] } else { Vec3::new(xz[i].x, ys[i], xz[i].y) }).collect();

    // 4. scenery and spawns keep their place against the generated rail
    let map = |p: Vec3| -> (f32, f32, Vec2, Vec2) {
        let (u, d) = gen.project(Vec2::new(p.x, p.z));
        let (q, dir) = new.at(u);
        (u, d * s, q, side(dir))
    };
    let mut props = vec![];
    let mut taken: Vec<Vec2> = vec![];
    for p in &g.props {
        if props.len() >= o.max_props {
            break;
        }
        let (u, d, q, n) = map(p.pos);
        if envelope(u) < 0.3 || d.abs() < o.path + 1.0 || d.abs() > o.band {
            continue;
        }
        let at = q + n * d;
        if taken.iter().any(|t| t.distance(at) < 1.5) {
            continue;
        }
        taken.push(at);
        props.push(PlacedProp { class: p.class.clone(), pos: at, yaw: p.yaw, scale: p.scale.clamp(0.6, 1.6) });
    }
    let mut spawns = vec![];
    for sp in &g.spawns {
        if spawns.len() >= o.max_spawns {
            break;
        }
        let (u, d, q, n) = map(sp.pos);
        if envelope(u) < 0.2 {
            continue;
        }
        // the generated view distances are far for this scale: keep them within easy photo range
        let d = d.signum() * d.abs().clamp(3.0, 9.0);
        let at = q + n * d;
        let to_rail = q - at;
        spawns.push(PlacedSpawn { air: sp.habitat == "air", pos: at, yaw: to_rail.x.atan2(to_rail.y) });
    }
    Ok(Plan { scale: s, rail: new_rail, height, layer: paint, props, spawns, rail_relief: (lo, hi), swing: swing_max })
}

/// The course's own trees and rocks are baked groups at the origin, and the game culls a model by its stored bounds,
/// so copies cannot stand in for single trees. The generated scenery is built instead: low-poly trees, rocks and bushes
/// as one OBJ (world positions relative to `anchor`), imported as one model group with the course's own materials.
/// Materials, sorted by name as the importer orders them: a_bark, b_leaf, c_rock, d_bush.
pub const SCENERY_MATERIALS: [(&str, &str); 4] = [
    ("a_bark", "MI_ENV_001_1_TreeA_com_barkA"),
    ("b_leaf", "MI_ENV_001_1_Hedge_hedgeA"),
    ("c_rock", "MI_ENV_001_1_Rock2_rock_bigA"),
    ("d_bush", "MI_ENV_001_1_Hedge_hedgeA"),
];

/// One prop of the built scenery: class, world position (on the ground), yaw, scale.
#[derive(Clone)]
pub struct BuiltProp {
    pub class: String,
    pub pos: Vec3,
    pub yaw: f32,
    pub scale: f32,
}

#[derive(Default)]
struct Obj {
    v: Vec<Vec3>,
    vt: Vec<Vec2>,
    /// (material, triangles of (v, vt) indices)
    faces: Vec<(usize, [usize; 3])>,
}

impl Obj {
    fn tri(&mut self, mat: usize, a: Vec3, b: Vec3, c: Vec3, uv: [Vec2; 3]) {
        let i = self.v.len();
        self.v.extend([a, b, c]);
        self.vt.extend(uv);
        self.faces.push((mat, [i, i + 1, i + 2]));
    }
    /// A ring-to-ring band: `rings` of (height, radius), `sides` around, turned by `yaw`, at `at`. `jitter` roughens
    /// radii (rocks).
    fn lathe(&mut self, mat: usize, at: Vec3, yaw: f32, rings: &[(f32, f32)], sides: usize, jitter: &dyn Fn(usize, usize) -> f32) {
        let p = |r: usize, k: usize| -> Vec3 {
            let (h, rad) = rings[r];
            let a = yaw + k as f32 / sides as f32 * std::f32::consts::TAU;
            let rad = rad * jitter(r, k % sides);
            at + Vec3::new(a.cos() * rad, h, a.sin() * rad)
        };
        for r in 0..rings.len() - 1 {
            for k in 0..sides {
                let (u0, u1) = (k as f32 / sides as f32 * 2.0, (k + 1) as f32 / sides as f32 * 2.0);
                let (v0, v1) = (rings[r].0 * 0.5, rings[r + 1].0 * 0.5);
                let (a, b, c, d) = (p(r, k), p(r, k + 1), p(r + 1, k + 1), p(r + 1, k));
                // outward winding (counter-clockwise seen from outside)
                if (b - a).cross(d - a).length_squared() > 1e-10 {
                    self.tri(mat, a, d, b, [Vec2::new(u0, v0), Vec2::new(u0, v1), Vec2::new(u1, v0)]);
                }
                if (c - b).cross(d - b).length_squared() > 1e-10 {
                    self.tri(mat, b, d, c, [Vec2::new(u1, v0), Vec2::new(u0, v1), Vec2::new(u1, v1)]);
                }
            }
        }
    }
}

/// The scenery OBJ and its MTL. Positions are relative to `anchor`.
pub fn scenery_obj(props: &[BuiltProp], anchor: Vec3, mtl_name: &str) -> (String, String, Vec<String>) {
    let mut o = Obj::default();
    let one = |_: usize, _: usize| 1.0;
    for (n, p) in props.iter().enumerate() {
        let s = p.scale;
        let at = p.pos - anchor;
        let rough = move |r: usize, k: usize| -> f32 {
            // fixed pseudo-random roughness per vertex, so a rock is not a perfect solid
            let h = ((n as u32).wrapping_mul(2654435761) ^ (r as u32 * 97 + k as u32 * 31)).wrapping_mul(2246822519);
            0.8 + (h >> 24) as f32 / 255.0 * 0.4
        };
        match p.class.as_str() {
            "tree" => {
                o.lathe(0, at, p.yaw, &[(-0.2, 0.28 * s), (1.6 * s, 0.22 * s), (1.6 * s, 0.0)], 6, &one);
                o.lathe(1, at, p.yaw, &[(1.2 * s, 0.0), (1.2 * s, 1.5 * s), (2.9 * s, 0.9 * s), (2.9 * s, 0.0)], 8, &one);
                o.lathe(1, at, p.yaw + 0.4, &[(2.4 * s, 0.0), (2.4 * s, 1.15 * s), (4.6 * s, 0.0)], 8, &one);
            }
            "rock" => {
                let r = 0.8 * s;
                o.lathe(2, at, p.yaw, &[(-0.3 * s, 0.0), (-0.3 * s, r * 0.9), (0.25 * s, r), (0.6 * s, r * 0.65), (0.75 * s, 0.0)], 7, &rough);
            }
            "bush" => {
                let r = 0.7 * s;
                o.lathe(3, at, p.yaw, &[(-0.1, 0.0), (-0.1, r * 0.8), (0.45 * s, r), (0.9 * s, r * 0.6), (1.05 * s, 0.0)], 7, &rough);
            }
            _ => {}
        }
    }
    let mut obj = format!("# random course scenery, made by the Snap level editor\nmtllib {mtl_name}\n");
    for v in &o.v {
        obj.push_str(&format!("v {:.4} {:.4} {:.4}\n", v.x, v.y, v.z));
    }
    for t in &o.vt {
        obj.push_str(&format!("vt {:.4} {:.4}\n", t.x, t.y));
    }
    let mut mats = vec![];
    let mut mtl = String::new();
    for (mi, (name, game)) in SCENERY_MATERIALS.iter().enumerate() {
        let tris: Vec<&[usize; 3]> = o.faces.iter().filter(|f| f.0 == mi).map(|f| &f.1).collect();
        if tris.is_empty() {
            continue;
        }
        mtl.push_str(&format!("newmtl {name}\n"));
        mats.push(game.to_string());
        obj.push_str(&format!("o {name}\nusemtl {name}\n"));
        for t in tris {
            obj.push_str(&format!("f {}/{} {}/{} {}/{}\n", t[0] + 1, t[0] + 1, t[1] + 1, t[1] + 1, t[2] + 1, t[2] + 1));
        }
    }
    (obj, mtl, mats)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tiny() -> String {
        let res = 4;
        let h: Vec<String> = (0..res * res).map(|i| ((i % res) as i64 * 1024 * 10).to_string()).collect();
        let sp: Vec<String> = (0..res * res).map(|_| "[0,0,255,0,0,0,0,0]".to_string()).collect();
        let samples: Vec<String> = (0..11)
            .map(|k| format!("{{\"distance_q\":{},\"position_q\":[{},0,{}]}}", k * 10 * 1024, (150 + if k == 5 { 20 } else { 0 }) * 1024, k * 10 * 1024))
            .collect();
        format!(
            "{{\"format_version\":2,\"seed\":7,\"params\":{{\"biome\":\"forest\"}},\"terrain\":{{\"resolution\":{res},\"width_q\":{},\"depth_q\":{},\"heights_q\":[{}],\"splat\":[{}]}},\"rail\":{{\"samples\":[{}]}},\"scenery\":[{{\"class\":\"rock\",\"position_q\":[{},0,{}],\"yaw_millidegrees\":90000,\"scale_permille\":1000}}],\"spawns\":[{{\"habitat\":\"air\",\"position_q\":[{},0,{}]}}]}}",
            300 * 1024,
            100 * 1024,
            h.join(","),
            sp.join(","),
            samples.join(","),
            140 * 1024,
            50 * 1024,
            160 * 1024,
            40 * 1024
        )
    }

    #[test]
    fn reads_format_2() {
        let g = Gen::parse(&tiny()).unwrap();
        assert_eq!(g.seed, 7);
        assert_eq!(g.biome, "forest");
        assert!((g.rail_length() - 100.0).abs() < 1e-3);
        assert!((g.height(150.0, 50.0) - 15.0).abs() < 1e-3);
        assert!((g.green(10.0, 10.0) - 1.0).abs() < 1e-6);
        assert!((g.props[0].yaw - std::f32::consts::FRAC_PI_2).abs() < 1e-4);
        assert!(Gen::parse("{\"format_version\":1}").is_err());
    }

    #[test]
    fn spine_projects_both_sides() {
        let s = Spine::new(vec![Vec2::new(0.0, 0.0), Vec2::new(0.0, 10.0)]);
        let (u, d) = s.project(Vec2::new(-2.0, 5.0));
        let (u2, d2) = s.project(Vec2::new(2.0, 5.0));
        assert!((u - 0.5).abs() < 1e-5 && (u2 - 0.5).abs() < 1e-5);
        assert!((d.abs() - 2.0).abs() < 1e-5 && d * d2 < 0.0);
        let (p, dir) = s.at(0.25);
        assert!((p - Vec2::new(0.0, 2.5)).length() < 1e-5 && (dir - Vec2::Y).length() < 1e-5);
    }

    #[test]
    fn plan_keeps_rail_ends_and_point_count() {
        let g = Gen::parse(&tiny()).unwrap();
        let n = 20;
        let t = Terrain { x0: -20.0, z0: -5.0, cell: 1.0, nx: 41, nz: 31, base: vec![0.0; 41 * 31], delta: vec![0.0; 41 * 31], layer: vec![0.0; 41 * 31], models: vec![] };
        let rail: Vec<Vec3> = (0..n).map(|i| Vec3::new(0.0, 1.0, i as f32)).collect();
        let p = plan(&g, &rail, &t, Opts::default(), &|_, _, _| None).unwrap();
        // a wall one unit to the side keeps the rail where it was
        let walled = plan(&g, &rail, &t, Opts::default(), &|_, _, _| Some(1.0)).unwrap();
        assert!(walled.rail.iter().zip(&rail).all(|(a, b)| (a.x - b.x).abs() < 1e-4 && (a.z - b.z).abs() < 1e-4));
        assert_eq!(p.rail.len(), n);
        assert_eq!(p.rail[0], rail[0]);
        assert_eq!(p.rail[n - 1], rail[n - 1]);
        // the generated rail bulges sideways in the middle, so the new rail does too
        assert!(p.rail[n / 2].x.abs() > 0.3, "{:?}", p.rail[n / 2]);
        assert!(!p.height.is_empty() && !p.layer.is_empty());
        assert!(p.height.iter().any(|h| h.1.abs() > 0.1));
        assert_eq!(p.spawns.len(), 1);
        assert!(p.spawns[0].air);
    }

    #[test]
    fn scenery_obj_has_one_object_per_used_material() {
        let props = vec![
            BuiltProp { class: "tree".into(), pos: Vec3::new(10.0, 1.0, 5.0), yaw: 0.3, scale: 1.0 },
            BuiltProp { class: "rock".into(), pos: Vec3::new(12.0, 1.0, 5.0), yaw: 0.0, scale: 1.2 },
        ];
        let (obj, mtl, mats) = scenery_obj(&props, Vec3::new(10.0, 1.0, 5.0), "s.mtl");
        assert_eq!(mats, vec!["MI_ENV_001_1_TreeA_com_barkA", "MI_ENV_001_1_Hedge_hedgeA", "MI_ENV_001_1_Rock2_rock_bigA"]);
        assert_eq!(mtl.matches("newmtl").count(), 3);
        assert_eq!(obj.matches("\no ").count(), 3);
        let nv = obj.lines().filter(|l| l.starts_with("v ")).count();
        let nf = obj.lines().filter(|l| l.starts_with("f ")).count();
        assert_eq!(nv, nf * 3);
        // the tree stands at the anchor: its lowest point is just below it
        let ys: Vec<f32> = obj.lines().filter(|l| l.starts_with("v ")).take(36).map(|l| l.split(' ').nth(2).unwrap().parse().unwrap()).collect();
        assert!(ys.iter().cloned().fold(f32::MAX, f32::min) > -0.3);
    }
}
