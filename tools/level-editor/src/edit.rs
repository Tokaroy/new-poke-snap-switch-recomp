//! Editing: every change is a list of float writes into a level's working bytes, so undo is exact, nothing outside
//! the edited floats changes, and an unedited export is byte-identical to the original entry.
//!
//! Export writes the changed levels as an opt-in mod pack in the loader's layout
//! (`<mods root>/<pack>/nx/Pack/LevelComposite/master/<LC>.drp/<entry>.levelb`, see docs/mods.md).

use crate::course::LoadedCourse;
use crate::pick::Ground;
use glam::{EulerRot, Quat, Vec3};
use std::path::{Path, PathBuf};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Sel {
    /// (level index, spawn index)
    Spawn(usize, usize),
    /// index into `LoadedCourse::models`
    Model(usize),
    /// (level index, rail segment, point index) of a point of the camera rail
    Rail(usize, usize, usize),
}

/// Level index and path index of the camera rail.
pub fn rail_ref(c: &LoadedCourse) -> Option<(usize, usize)> {
    c.levels.iter().enumerate().find_map(|(li, l)| {
        let e = l.rail?;
        l.paths.iter().position(|p| p.entity == e).map(|pi| (li, pi))
    })
}

/// Default number of neighbouring rail points (each side) that follow a dragged point.
pub const RAIL_SOFT: usize = 3;

/// The rail as one chain of nodes; a node groups the (segment, index) points at the same place (segment joints).
fn rail_chain(p: &crate::level::PathData) -> Vec<Vec<(usize, usize)>> {
    let mut chain: Vec<Vec<(usize, usize)>> = vec![];
    let mut last: Option<[f32; 3]> = None;
    for (si, seg) in p.segments.iter().enumerate() {
        for (i, pt) in seg.iter().enumerate() {
            match last {
                Some(l) if crate::level::dist(l, *pt) < 1e-3 => chain.last_mut().unwrap().push((si, i)),
                _ => chain.push(vec![(si, i)]),
            }
            last = Some(*pt);
        }
    }
    chain
}

/// Writes that move rail point (seg, idx) by `delta`; the `soft` points on each side follow with a smooth falloff, so the
/// ride bends instead of kinking. Points shared by two segments move together.
pub fn move_rail(c: &LoadedCourse, l: usize, seg: usize, idx: usize, delta: Vec3, soft: usize) -> Result<Writes, String> {
    let (rl, pi) = rail_ref(c).ok_or("this course has no rail")?;
    if rl != l {
        return Err("not a rail point".into());
    }
    let p = &c.levels[l].paths[pi];
    let chain = rail_chain(p);
    let at = chain.iter().position(|n| n.contains(&(seg, idx))).ok_or("no such rail point")?;
    let mut out = vec![];
    for (k, node) in chain.iter().enumerate() {
        let dist = k.abs_diff(at);
        if dist > soft {
            continue;
        }
        let w = 0.5 * (1.0 + (std::f32::consts::PI * dist as f32 / (soft + 1) as f32).cos());
        for &(s, i) in node {
            out.extend(w3(l, p.offs[s][i], Vec3::from(p.segments[s][i]) + delta * w));
        }
    }
    Ok(out)
}

/// The rail as it is in the original file (same shape as the current one).
pub fn rail_original(c: &LoadedCourse) -> Option<Vec<Vec<[f32; 3]>>> {
    let (l, pi) = rail_ref(c)?;
    let lv = &c.levels[l];
    let f = |o: usize| lv.orig.get(o..o + 4).map(|b| f32::from_le_bytes(b.try_into().unwrap())).unwrap_or(0.0);
    Some(lv.paths[pi].offs.iter().map(|s| s.iter().map(|&o| [f(o), f(o + 4), f(o + 8)]).collect()).collect())
}

/// Writes that put every rail point back where the original file has it.
pub fn reset_rail(c: &LoadedCourse) -> Result<Writes, String> {
    let (l, pi) = rail_ref(c).ok_or("this course has no rail")?;
    let orig = rail_original(c).ok_or("no rail")?;
    let p = &c.levels[l].paths[pi];
    let mut out = vec![];
    for (s, seg) in orig.iter().enumerate() {
        for (i, pt) in seg.iter().enumerate() {
            if p.segments[s][i] != *pt {
                out.extend(w3(l, p.offs[s][i], Vec3::from(*pt)));
            }
        }
    }
    Ok(out)
}

pub struct RailStats {
    pub length: f32,
    pub orig_length: f32,
    /// (segment, index, turn in degrees, original turn) of points that bend more sharply than before
    pub sharp: Vec<(usize, usize, f32, f32)>,
    /// (segment, index) of neighbouring points closer than 0.25 units
    pub bunched: Vec<(usize, usize)>,
}

/// Camera speed along the rail in units per second, measured on Florio leg 1: part 0 (115.74 units) took 141.0 s in
/// three unedited runs, and 116.49 units took 142.0 s with a bend (2026-11-14). The game keeps the speed and rides the
/// extra length, so a length change turns into a time change at this rate.
pub const RAIL_SPEED: f32 = 0.82;

impl RailStats {
    /// Estimated change in ride time against the original rail, in seconds.
    pub fn seconds_change(&self) -> f32 {
        (self.length - self.orig_length) / RAIL_SPEED
    }
    /// One plain sentence a beginner can read: length, and ride time against the original. Only the change is given in
    /// seconds, because the speed was measured on part 0 and the short later parts were not timed.
    pub fn summary(&self) -> String {
        let d = self.seconds_change();
        let change = if d.abs() < 0.05 {
            "the ride takes the same time as the original".to_string()
        } else {
            format!("the ride takes about {:.1} s {} than the original", d.abs(), if d > 0.0 { "longer" } else { "shorter" })
        };
        format!("Rail length {:.1} units: {change}", self.length)
    }
}

fn seg_len(s: &[[f32; 3]]) -> f32 {
    s.windows(2).map(|w| crate::level::dist(w[0], w[1])).sum()
}

fn turn(s: &[[f32; 3]], i: usize) -> f32 {
    if i == 0 || i + 1 >= s.len() {
        return 0.0;
    }
    let a = Vec3::from(s[i]) - Vec3::from(s[i - 1]);
    let b = Vec3::from(s[i + 1]) - Vec3::from(s[i]);
    if a.length_squared() < 1e-8 || b.length_squared() < 1e-8 {
        return 0.0;
    }
    a.angle_between(b).to_degrees()
}

/// Length and smoothness of the ride compared with the original rail. A turn counts as sharp when it passes 35 degrees
/// and is at least 10 degrees sharper than the original turn at that point.
pub fn rail_stats(c: &LoadedCourse) -> Option<RailStats> {
    let (l, pi) = rail_ref(c)?;
    let cur = &c.levels[l].paths[pi].segments;
    let orig = rail_original(c)?;
    let mut st = RailStats { length: 0.0, orig_length: 0.0, sharp: vec![], bunched: vec![] };
    for (s, seg) in cur.iter().enumerate() {
        st.length += seg_len(seg);
        st.orig_length += seg_len(&orig[s]);
        for i in 0..seg.len() {
            let (t, t0) = (turn(seg, i), turn(&orig[s], i));
            if t > 35.0 && t > t0 + 10.0 {
                st.sharp.push((s, i, t, t0));
            }
            if i + 1 < seg.len() && crate::level::dist(seg[i], seg[i + 1]) < 0.25 && crate::level::dist(orig[s][i], orig[s][i + 1]) >= 0.25 {
                st.bunched.push((s, i));
            }
        }
    }
    Some(st)
}

#[derive(Clone, Debug)]
struct FloatWrite {
    level: usize,
    off: usize,
    old: f32,
    new: f32,
}

pub struct Edit {
    pub label: String,
    key: Option<String>,
    writes: Vec<FloatWrite>,
}

/// (level index, byte offset, new value)
pub type Writes = Vec<(usize, usize, f32)>;

#[derive(Default)]
pub struct History {
    undo: Vec<Edit>,
    redo: Vec<Edit>,
    merge_open: bool,
    /// Bumped on every change; the UI rebuilds overlays and checks when it moves.
    pub version: u64,
}

impl History {
    /// Applies writes as one undo step. With a `key`, consecutive applies with the same key merge into one step until
    /// `end_interaction` (one drag = one undo step).
    pub fn apply(&mut self, c: &mut LoadedCourse, label: &str, key: Option<&str>, writes: Writes) {
        if writes.is_empty() {
            return;
        }
        let merge = self.merge_open && key.is_some() && self.undo.last().map(|e| e.key.as_deref()) == Some(key);
        if !merge {
            self.undo.push(Edit { label: label.to_string(), key: key.map(str::to_string), writes: vec![] });
        }
        let edit = self.undo.last_mut().unwrap();
        let mut touched = vec![];
        for (level, off, new) in writes {
            let Some(old) = read_f32(c, level, off) else { continue };
            write_f32(c, level, off, new);
            match edit.writes.iter_mut().find(|w| w.level == level && w.off == off) {
                Some(w) => w.new = new,
                None => edit.writes.push(FloatWrite { level, off, old, new }),
            }
            touched.push(level);
        }
        self.merge_open = key.is_some();
        self.redo.clear();
        self.version += 1;
        sync(c, &touched);
    }

    pub fn end_interaction(&mut self) {
        self.merge_open = false;
    }

    pub fn undo(&mut self, c: &mut LoadedCourse) -> Option<String> {
        let e = self.undo.pop()?;
        let mut touched = vec![];
        for w in e.writes.iter().rev() {
            write_f32(c, w.level, w.off, w.old);
            touched.push(w.level);
        }
        sync(c, &touched);
        let label = e.label.clone();
        self.redo.push(e);
        self.merge_open = false;
        self.version += 1;
        Some(label)
    }

    pub fn redo(&mut self, c: &mut LoadedCourse) -> Option<String> {
        let e = self.redo.pop()?;
        let mut touched = vec![];
        for w in &e.writes {
            write_f32(c, w.level, w.off, w.new);
            touched.push(w.level);
        }
        sync(c, &touched);
        let label = e.label.clone();
        self.undo.push(e);
        self.merge_open = false;
        self.version += 1;
        Some(label)
    }

    pub fn undo_label(&self) -> Option<&str> {
        self.undo.last().map(|e| e.label.as_str())
    }
    pub fn redo_label(&self) -> Option<&str> {
        self.redo.last().map(|e| e.label.as_str())
    }
    pub fn steps(&self) -> usize {
        self.undo.len()
    }
}

/// A float of a level, or a terrain grid cell for the pseudo levels in `ground`.
fn read_f32(c: &LoadedCourse, level: usize, off: usize) -> Option<f32> {
    match c.levels.get(level) {
        Some(l) => Some(l.f32_at(off)),
        None => c.terrain.as_ref()?.get(level, off),
    }
}
fn write_f32(c: &mut LoadedCourse, level: usize, off: usize, v: f32) {
    match c.levels.get_mut(level) {
        Some(l) => l.write_f32(off, v),
        None => {
            if let Some(t) = c.terrain.as_mut() {
                t.set(level, off, v);
            }
        }
    }
}

/// Re-reads edited levels and copies model placements into the placed-model list.
fn sync(c: &mut LoadedCourse, touched: &[usize]) {
    let mut t: Vec<usize> = touched.iter().copied().filter(|&l| l < c.levels.len()).collect();
    t.sort();
    t.dedup();
    for &li in &t {
        let l = &mut c.levels[li];
        l.refresh();
        l.dirty = l.data != l.orig || l.has_adds();
    }
    for m in c.models.iter_mut().filter(|m| t.contains(&m.level_idx)) {
        m.inst = c.levels[m.level_idx].models[m.inst_idx].clone();
    }
}

fn w3(level: usize, off: usize, v: Vec3) -> Writes {
    vec![(level, off, v.x), (level, off + 4, v.y), (level, off + 8, v.z)]
}
fn wq(level: usize, off: usize, q: Quat) -> Writes {
    vec![(level, off, q.x), (level, off + 4, q.y), (level, off + 8, q.z), (level, off + 12, q.w)]
}

pub fn position(c: &LoadedCourse, s: Sel) -> Option<Vec3> {
    match s {
        Sel::Spawn(l, i) => c.levels.get(l)?.spawns.get(i).map(|s| Vec3::from(s.pos)),
        Sel::Model(m) => c.models.get(m).map(|m| Vec3::from(m.inst.pos)),
        Sel::Rail(l, s, i) => {
            let (rl, pi) = rail_ref(c)?;
            (rl == l).then_some(())?;
            c.levels[l].paths[pi].segments.get(s)?.get(i).map(|p| Vec3::from(*p))
        }
    }
}

/// How a selection moves, in words, or why it cannot.
pub fn move_mode(c: &LoadedCourse, s: Sel) -> Result<String, String> {
    match s {
        Sel::Spawn(l, i) => {
            let sp = &c.levels[l].spawns[i];
            if !sp.pos_from_path && sp.pos_off.is_some() {
                Ok("moves its spawn point".into())
            } else if let Some(p) = sp.path_idx {
                let shared = c.levels[l].spawns.iter().filter(|o| o.path_idx == Some(p)).count();
                let pts: usize = c.levels[l].paths[p].segments.iter().map(|s| s.len()).sum();
                Ok(if shared > 1 {
                    format!("moves its whole route ({pts} points, shared with {} other spawns)", shared - 1)
                } else {
                    format!("moves its whole route ({pts} points)")
                })
            } else {
                Err("its position is not stored in the file, so it cannot be moved in place yet".into())
            }
        }
        Sel::Model(m) => match c.models[m].inst.pos_off {
            Some(_) => Ok("moves the model instance".into()),
            None => Err("its position is not stored in the file, so it cannot be moved in place yet".into()),
        },
        Sel::Rail(..) => Ok("bends the ride; nearby rail points follow so it stays smooth".into()),
    }
}

/// Writes that move a selection by `delta`.
pub fn move_by(c: &LoadedCourse, s: Sel, delta: Vec3) -> Result<Writes, String> {
    move_mode(c, s)?;
    match s {
        Sel::Spawn(l, i) => {
            let lv = &c.levels[l];
            let sp = &lv.spawns[i];
            if !sp.pos_from_path {
                if let Some(o) = sp.pos_off {
                    return Ok(w3(l, o, Vec3::from(sp.pos) + delta));
                }
            }
            let p = &lv.paths[sp.path_idx.unwrap()];
            let mut out = vec![];
            for (seg, offs) in p.segments.iter().zip(&p.offs) {
                for (pt, &o) in seg.iter().zip(offs) {
                    out.extend(w3(l, o, Vec3::from(*pt) + delta));
                }
            }
            Ok(out)
        }
        Sel::Model(m) => {
            let pm = &c.models[m];
            Ok(w3(pm.level_idx, pm.inst.pos_off.unwrap(), Vec3::from(pm.inst.pos) + delta))
        }
        Sel::Rail(l, s, i) => move_rail(c, l, s, i, delta, RAIL_SOFT),
    }
}

/// True for objects added in the editor (they can be removed again).
pub fn is_added(c: &LoadedCourse, s: Sel) -> bool {
    match s {
        Sel::Spawn(l, i) => c.levels[l].spawns[i].alive_off.is_some(),
        Sel::Model(m) => c.models[m].inst.alive_off.is_some(),
        Sel::Rail(..) => false,
    }
}

/// Makes a copy of a selection at `at` (the source position when None). Returns the new selection and the writes that
/// make it present; undoing them hides it again.
pub fn add_copy(c: &mut LoadedCourse, s: Sel, at: Option<Vec3>) -> Result<(Sel, Writes), String> {
    match s {
        Sel::Spawn(l, i) => {
            let ni = c.levels[l].add_spawn_slot(i)?;
            let sp = &c.levels[l].spawns[ni];
            let base = sp.alive_off.unwrap();
            let mut w = vec![(l, base, 1.0)];
            w.extend(w3(l, sp.pos_off.unwrap(), at.unwrap_or(Vec3::from(sp.pos))));
            Ok((Sel::Spawn(l, ni), w))
        }
        Sel::Model(m) => {
            let (li, src_inst) = (c.models[m].level_idx, c.models[m].inst_idx);
            let ni = c.levels[li].add_model_slot(src_inst);
            let inst = c.levels[li].models[ni].clone();
            let src = &c.models[m];
            let pm = crate::course::PlacedModel {
                inst: inst.clone(),
                level: src.level.clone(),
                level_idx: li,
                inst_idx: ni,
                source: src.source.clone(),
                mesh: src.mesh.clone(),
                error: src.error.clone(),
                visible: true,
            };
            c.models.push(pm);
            let base = inst.alive_off.unwrap();
            let mut w = vec![(li, base, 1.0)];
            w.extend(w3(li, inst.pos_off.unwrap(), at.unwrap_or(Vec3::from(inst.pos))));
            Ok((Sel::Model(c.models.len() - 1), w))
        }
        Sel::Rail(..) => Err("rail points cannot be copied".into()),
    }
}

/// A copy at `at` with its own rotation (when given and stored) and, for scenery models, its size multiplied by
/// `scale_mul`. One undo step hides it again.
pub fn place_copy(c: &mut LoadedCourse, s: Sel, at: Vec3, rot: Option<Quat>, scale_mul: f32) -> Result<(Sel, Writes), String> {
    let (ns, mut w) = add_copy(c, s, Some(at))?;
    match ns {
        Sel::Spawn(l, i) => {
            if let (Some(q), Some(o)) = (rot, c.levels[l].spawns[i].rot_off) {
                w.extend(wq(l, o, q));
            }
        }
        Sel::Model(m) => {
            let pm = &c.models[m];
            if let (Some(q), Some(o)) = (rot, pm.inst.rot_off) {
                w.extend(wq(pm.level_idx, o, q));
            }
            if let (true, Some(o)) = ((scale_mul - 1.0).abs() > 1e-6, pm.inst.scale_off) {
                w.extend(w3(pm.level_idx, o, Vec3::from(pm.inst.scale) * scale_mul));
            }
        }
        Sel::Rail(..) => {}
    }
    Ok((ns, w))
}

/// Writes that remove an added object (undo brings it back). Objects from the original file cannot be removed yet.
pub fn remove(c: &LoadedCourse, s: Sel) -> Result<Writes, String> {
    let (l, off) = match s {
        Sel::Spawn(l, i) => (l, c.levels[l].spawns[i].alive_off),
        Sel::Model(m) => (c.models[m].level_idx, c.models[m].inst.alive_off),
        Sel::Rail(..) => return Err("rail points cannot be removed".into()),
    };
    off.map(|o| vec![(l, o, 0.0)]).ok_or_else(|| "only objects you added can be removed for now".to_string())
}

pub fn yaw_of(rot: [f32; 4]) -> f32 {
    let q = Quat::from_array(rot);
    if q.length_squared() < 1e-6 {
        return 0.0;
    }
    q.normalize().to_euler(EulerRot::YXZ).0
}

/// Writes that set the facing (rotation about the up axis), or None when rotation is not stored.
pub fn set_yaw(c: &LoadedCourse, s: Sel, yaw: f32) -> Option<Writes> {
    let q = Quat::from_rotation_y(yaw);
    match s {
        Sel::Spawn(l, i) => c.levels[l].spawns[i].rot_off.map(|o| wq(l, o, q)),
        Sel::Model(m) => {
            let pm = &c.models[m];
            // keep the model's tilt, replace only the heading
            let old = Quat::from_array(pm.inst.rot);
            let (_, x, z) = if old.length_squared() < 1e-6 { (0.0, 0.0, 0.0) } else { old.normalize().to_euler(EulerRot::YXZ) };
            let q = Quat::from_euler(EulerRot::YXZ, yaw, x, z);
            pm.inst.rot_off.map(|o| wq(pm.level_idx, o, q))
        }
        Sel::Rail(..) => None,
    }
}

pub fn rot_of(c: &LoadedCourse, s: Sel) -> Option<[f32; 4]> {
    match s {
        Sel::Spawn(l, i) => c.levels[l].spawns[i].rot_off.map(|_| c.levels[l].spawns[i].rot),
        Sel::Model(m) => c.models[m].inst.rot_off.map(|_| c.models[m].inst.rot),
        Sel::Rail(..) => None,
    }
}

pub fn set_scale(c: &LoadedCourse, m: usize, scale: Vec3) -> Option<Writes> {
    let pm = &c.models[m];
    pm.inst.scale_off.map(|o| w3(pm.level_idx, o, scale))
}

/// Heading that makes a spawn face the nearest rail point.
pub fn yaw_facing_rail(c: &LoadedCourse, p: Vec3) -> Option<f32> {
    let rail = c.rail()?;
    let q = rail.segments.iter().flatten().map(|x| Vec3::from(*x)).min_by(|a, b| {
        a.distance_squared(p).partial_cmp(&b.distance_squared(p)).unwrap_or(std::cmp::Ordering::Equal)
    })?;
    let d = q - p;
    (d.x.abs() + d.z.abs() > 1e-3).then(|| d.x.atan2(d.z))
}

pub struct Issue {
    pub sel: Sel,
    pub text: String,
}

/// Plain-language checks a beginner can act on.
pub fn checks(c: &LoadedCourse, g: &Ground) -> Vec<Issue> {
    let mut out = vec![];
    let rail: Vec<Vec3> = c.rail().map(|r| r.segments.iter().flatten().map(|p| Vec3::from(*p)).collect()).unwrap_or_default();
    for (li, l) in c.levels.iter().enumerate() {
        for (si, s) in l.spawns.iter().enumerate() {
            let p = Vec3::from(s.pos);
            let sel = Sel::Spawn(li, si);
            if !s.alive {
                continue;
            }
            // No stored position and no route: the game places it some other way, so there is nothing to check.
            if p == Vec3::ZERO && !s.pos_from_path {
                continue;
            }
            if !g.models.is_empty() && g.is_under(p) {
                out.push(Issue { sel, text: format!("{} at ({:.1}, {:.1}, {:.1}) may be under the ground", s.label, p.x, p.y, p.z) });
            }
            if let Some(d) = rail.iter().map(|r| r.distance(p)).reduce(f32::min) {
                if d > 120.0 {
                    out.push(Issue { sel, text: format!("{} is {:.0} units from the rail; the player may never see it", s.label, d) });
                }
            }
        }
    }
    // The ride: length (time), sharp new bends, bunched points, rail under the ground.
    if let (Some(st), Some((rl, pi)), Some(orig)) = (rail_stats(c), rail_ref(c), rail_original(c)) {
        let start = Sel::Rail(rl, 0, 0);
        let pct = (st.length / st.orig_length.max(1e-3) - 1.0) * 100.0;
        if pct.abs() > 10.0 {
            out.push(Issue {
                sel: start,
                text: format!(
                    "The rail is {:.0}% {} than the original ({:.1} vs {:.1} units): the ride takes about {:.0} s {}",
                    pct.abs(),
                    if pct > 0.0 { "longer" } else { "shorter" },
                    st.length,
                    st.orig_length,
                    st.seconds_change().abs(),
                    if pct > 0.0 { "more" } else { "less" }
                ),
            });
        }
        for (s, i, t, t0) in &st.sharp {
            out.push(Issue { sel: Sel::Rail(rl, *s, *i), text: format!("Sharp bend at rail point {i}: {t:.0}° (was {t0:.0}°); the camera will jerk") });
        }
        for (s, i) in &st.bunched {
            out.push(Issue { sel: Sel::Rail(rl, *s, *i), text: format!("Rail points {i} and {} are almost on top of each other", i + 1) });
        }
        if !g.models.is_empty() {
            for (s, seg) in c.levels[rl].paths[pi].segments.iter().enumerate() {
                for (i, p) in seg.iter().enumerate() {
                    if p != &orig[s][i] && g.is_under(Vec3::from(*p)) && !g.is_under(Vec3::from(orig[s][i])) {
                        out.push(Issue { sel: Sel::Rail(rl, s, i), text: format!("Rail point {i} is under the ground") });
                    }
                }
            }
        }
    }
    out
}

pub fn sanitize_pack(name: &str) -> String {
    let s: String = name.chars().map(|ch| if ch.is_ascii_alphanumeric() || ch == '-' || ch == '_' { ch } else { '-' }).collect();
    if s.trim_matches('-').is_empty() {
        "my-course".into()
    } else {
        s
    }
}

fn json_str(s: &str) -> String {
    let mut o = String::from("\"");
    for ch in s.chars() {
        match ch {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            c if (c as u32) < 0x20 => o.push_str(&format!("\\u{:04x}", c as u32)),
            c => o.push(c),
        }
    }
    o.push('"');
    o
}

/// Writes changed levels (or every level with `all`) as a mod pack. Returns the files written.
pub fn export(c: &LoadedCourse, mods_root: &Path, pack: &str, description: &str, all: bool) -> Result<Vec<PathBuf>, String> {
    let pack = sanitize_pack(pack);
    let dir = mods_root.join(&pack);
    let arch = dir.join(c.archive.replace('/', std::path::MAIN_SEPARATOR_STR));
    let levels: Vec<_> = c.levels.iter().filter(|l| all || l.dirty).collect();
    let ground = c.terrain.as_ref().filter(|t| t.edited());
    if levels.is_empty() && ground.is_none() {
        return Err("nothing to export: no level has changes".into());
    }
    std::fs::create_dir_all(&arch).map_err(|e| format!("{}: {e}", arch.display()))?;
    let mut written = vec![];
    if let Some(t) = ground {
        written.extend(t.export(c, &arch)?);
    }
    for l in levels {
        let fname = Path::new(&l.file).file_name().ok_or("bad level file name")?;
        let out = arch.join(fname);
        let bytes = l.export_bytes()?;
        std::fs::write(&out, &bytes).map_err(|e| format!("{}: {e}", out.display()))?;
        written.push(out);
    }
    let pj = dir.join("pack.json");
    let body = format!(
        "{{ \"name\": {}, \"version\": \"0.1.0\", \"author\": \"\", \"description\": {} }}\n",
        json_str(&pack),
        json_str(description)
    );
    std::fs::write(&pj, body).map_err(|e| format!("{}: {e}", pj.display()))?;
    written.push(pj);
    if let Some(mj) = enable_pack(mods_root, &pack)? {
        written.push(mj);
    }
    Ok(written)
}

/// Since 2026-11-13 the game loads only packs listed in `<mods root>/mods.json`. Adds `pack` to the list (creating the file
/// if needed) and leaves every other entry as it is. A pack already named in the file is left alone, so a pack the player
/// turned off stays off. Returns the file path when it was written.
pub fn enable_pack(mods_root: &Path, pack: &str) -> Result<Option<PathBuf>, String> {
    let mj = mods_root.join("mods.json");
    let entry = format!("{{ \"id\": {}, \"enabled\": true }}", json_str(pack));
    let body = match std::fs::read_to_string(&mj) {
        Err(_) => format!("{{ \"packs\": [\n  {entry}\n] }}\n"),
        Ok(txt) => {
            if txt.contains(&json_str(pack)) {
                return Ok(None);
            }
            let at = txt
                .find("\"packs\"")
                .and_then(|k| txt[k..].find('[').map(|b| k + b + 1))
                .ok_or_else(|| format!("{}: no \"packs\" list; add {entry} by hand", mj.display()))?;
            let empty = txt[at..].trim_start().starts_with(']');
            format!("{}\n  {entry}{}{}", &txt[..at], if empty { "" } else { "," }, &txt[at..])
        }
    };
    std::fs::write(&mj, body).map_err(|e| format!("{}: {e}", mj.display()))?;
    Ok(Some(mj))
}

/// The game's own mods folder (`%APPDATA%\SnapRecomp\mods`), or `SNAP_MODS_DIR` when set.
pub fn game_mods_dir() -> Option<PathBuf> {
    if let Ok(d) = std::env::var("SNAP_MODS_DIR") {
        return Some(PathBuf::from(d));
    }
    std::env::var("APPDATA").ok().map(|a| Path::new(&a).join("SnapRecomp").join("mods"))
}
