//! `levelb` reader (FlatBuffers, version 6). Field meaning comes from analysis of LC_001_A (Florio):
//!
//! - root slot 1 = entry name, slot 2 = entities (`{components[], hash}`), slot 3 = scene nodes
//!   (`{components[], node_id (s1), entity_hash (s2), s4}`), slot 6 = course parameters whose s0 is the scene
//!   node of the NEO-ONE rail, slot 11 = species numbers used in the map, slot 13 = environment parameters.
//! - a component is a table with a tag byte in s0 and exactly one sub-table in a higher slot; the slot says the
//!   kind: 5 model instance (name, pos, quat, scale), 6 character template (skeleton, animation archive),
//!   8 path (segments of points), 9 character name (`CH_nnn`), 10 spawn (pos, quat, records -> template + path node).
//!
//! Rail check: Florio leg 1 rail (s6.s0 -> node -> path) measures 115.7 units; the runtime log reads 0..116.

use crate::fb::{Buf, Table};
use std::collections::{BTreeMap, HashMap};

#[derive(Clone, Debug)]
pub struct ModelInstance {
    pub name: String,
    pub pos: [f32; 3],
    pub rot: [f32; 4],
    pub scale: [f32; 3],
    /// Byte offsets of the stored fields (None when the field is not in the file and so cannot be patched in place).
    pub pos_off: Option<usize>,
    pub rot_off: Option<usize>,
    pub scale_off: Option<usize>,
    /// Index of the owning entity in the root entity list (the clone source when adding a copy).
    pub entity: usize,
    /// Added in the editor: its floats live in the level's virtual region and `alive_off` holds its present flag.
    pub alive_off: Option<usize>,
    pub alive: bool,
}

#[derive(Clone, Debug)]
pub struct PathData {
    pub entity: u32,
    pub segments: Vec<Vec<[f32; 3]>>,
    /// Byte offset of every point, same shape as `segments`.
    pub offs: Vec<Vec<usize>>,
}

impl PathData {
    pub fn length(&self, seg: usize) -> f32 {
        self.segments
            .get(seg)
            .map(|s| s.windows(2).map(|w| dist(w[0], w[1])).sum())
            .unwrap_or(0.0)
    }
    pub fn start(&self) -> Option<[f32; 3]> {
        self.segments.iter().find_map(|s| s.first().copied())
    }
}

#[derive(Clone, Debug, Default)]
pub struct CharTemplate {
    pub ch_name: String,
    pub skeleton: String,
    pub anim: String,
}

#[derive(Clone, Debug)]
pub struct Spawn {
    pub entity: u32,
    pub pos: [f32; 3],
    pub rot: [f32; 4],
    /// Position came from the linked path's first point because the spawn's own position is the origin.
    pub pos_from_path: bool,
    /// Template entity hashes of the spawn records.
    pub templates: Vec<u32>,
    /// Path entity of the first record that names a scene node, if any.
    pub path: Option<u32>,
    pub label: String,
    pub pos_off: Option<usize>,
    pub rot_off: Option<usize>,
    /// Index into `Level::paths` of `path`.
    pub path_idx: Option<usize>,
    pub entity_idx: usize,
    pub alive_off: Option<usize>,
    pub alive: bool,
}

#[derive(Clone, Debug, Default)]
pub struct Level {
    pub name: String,
    pub file: String,
    pub size: usize,
    pub entity_count: usize,
    pub node_count: usize,
    pub component_kinds: BTreeMap<String, usize>,
    pub models: Vec<ModelInstance>,
    pub paths: Vec<PathData>,
    pub templates: HashMap<u32, CharTemplate>,
    pub spawns: Vec<Spawn>,
    pub rail: Option<u32>,
    pub species: Vec<u32>,
    pub env_params: Vec<String>,
    /// Working copy of the file. Edits patch floats in place, so unedited data stays byte-identical.
    pub data: Vec<u8>,
    /// The file as read, for change detection.
    pub orig: Vec<u8>,
    pub dirty: bool,
    /// Floats of objects added in the editor, addressed from `VBASE` so the same float writes (and undo) apply.
    pub extra: Vec<u8>,
}

/// Start of the virtual float region (offsets at or past it address `Level::extra`, not the file).
pub const VBASE: usize = 1 << 30;

pub fn dist(a: [f32; 3], b: [f32; 3]) -> f32 {
    ((a[0] - b[0]).powi(2) + (a[1] - b[1]).powi(2) + (a[2] - b[2]).powi(2)).sqrt()
}

pub fn kind_name(slot: usize) -> &'static str {
    match slot {
        1 => "light/probe (s1)",
        4 => "transform",
        5 => "model instance",
        6 => "character template",
        7 => "collision/trigger shape",
        8 => "path",
        9 => "character name",
        10 => "spawn",
        11 => "character behaviour",
        14 => "effect",
        15 => "decal/texture (s15)",
        _ => "unnamed",
    }
}

/// Returns (slot, sub-table) of a component.
fn component_body<'a>(c: &Table<'a>) -> Option<(usize, Table<'a>)> {
    (1..c.slot_count()).find_map(|s| c.table(s).map(|t| (s, t)))
}

pub fn parse(bytes: &[u8], file: &str) -> Result<Level, String> {
    let buf = Buf(bytes);
    let root = buf.root().ok_or("not a FlatBuffers file")?;
    let version = root.u32(0).unwrap_or(0);
    if version != 6 {
        return Err(format!("levelb version {version}, expected 6"));
    }
    let mut lv = Level {
        name: root.str(1).unwrap_or("?").to_string(),
        file: file.to_string(),
        size: bytes.len(),
        ..Default::default()
    };
    let entities = root.tables(2);
    let nodes = root.tables(3);
    lv.entity_count = entities.len();
    lv.node_count = nodes.len();

    // node id -> entity hash
    let node_entity: HashMap<u32, u32> =
        nodes.iter().filter_map(|n| Some((n.u32(1)?, n.u32(2)?))).collect();

    let mut path_by_entity: HashMap<u32, usize> = HashMap::new();
    type RawSpawn = (u32, [f32; 3], [f32; 4], Vec<(u32, Option<u32>)>, Option<usize>, Option<usize>, usize);
    let mut raw_spawns: Vec<RawSpawn> = vec![];

    for (ei, e) in entities.iter().enumerate() {
        let hash = e.u32(1).unwrap_or(0);
        let mut tmpl = CharTemplate::default();
        let mut is_tmpl = false;
        for c in e.tables(0) {
            let Some((slot, t)) = component_body(&c) else { continue };
            *lv.component_kinds.entry(format!("{:>2} {}", slot, kind_name(slot))).or_default() += 1;
            match slot {
                5 => lv.models.push(ModelInstance {
                    name: t.str(1).unwrap_or("").to_string(),
                    pos: t.floats::<3>(3).unwrap_or([0.0; 3]),
                    rot: t.floats::<4>(4).unwrap_or([0.0, 0.0, 0.0, 1.0]),
                    scale: t.floats::<3>(5).unwrap_or([1.0; 3]),
                    pos_off: t.field(3),
                    rot_off: t.field(4),
                    scale_off: t.field(5),
                    entity: ei,
                    alive_off: None,
                    alive: true,
                }),
                6 => {
                    is_tmpl = true;
                    tmpl.skeleton = t.str(1).unwrap_or("").to_string();
                    tmpl.anim = t.str(3).unwrap_or("").to_string();
                }
                9 => {
                    is_tmpl = true;
                    tmpl.ch_name = t.str(1).unwrap_or("").to_string();
                }
                8 => {
                    let mut segments = vec![];
                    let mut offs = vec![];
                    for seg in t.tables(1) {
                        let pts = seg.vec3s(1);
                        if pts.is_empty() {
                            continue;
                        }
                        let (p0, _) = seg.vec(1).unwrap_or((0, 0));
                        offs.push((0..pts.len()).map(|i| p0 + 12 * i).collect());
                        segments.push(pts);
                    }
                    if !segments.is_empty() {
                        path_by_entity.insert(hash, lv.paths.len());
                        lv.paths.push(PathData { entity: hash, segments, offs });
                    }
                }
                10 => {
                    let pos = t.floats::<3>(1).unwrap_or([0.0; 3]);
                    let rot = t.floats::<4>(2).unwrap_or([0.0, 0.0, 0.0, 1.0]);
                    let recs = t
                        .tables(3)
                        .iter()
                        .map(|r| (r.u32(0).unwrap_or(0), r.u32(2).and_then(|n| node_entity.get(&n).copied())))
                        .collect();
                    raw_spawns.push((hash, pos, rot, recs, t.field(1), t.field(2), ei));
                }
                _ => {}
            }
        }
        if is_tmpl {
            lv.templates.insert(hash, tmpl);
        }
    }

    for (entity, pos, rot, recs, pos_off, rot_off, entity_idx) in raw_spawns {
        let path = recs.iter().find_map(|r| r.1.filter(|p| path_by_entity.contains_key(p)));
        let mut p = pos;
        let mut from_path = false;
        if pos == [0.0; 3] {
            if let Some(start) = path.and_then(|pe| lv.paths[path_by_entity[&pe]].start()) {
                p = start;
                from_path = true;
            }
        }
        let templates: Vec<u32> = recs.iter().map(|r| r.0).collect();
        let mut names: Vec<String> = templates
            .iter()
            .map(|h| match lv.templates.get(h) {
                Some(t) if !t.ch_name.is_empty() => t.ch_name.clone(),
                Some(t) => t.skeleton.clone(),
                None => format!("#{h:08x}"),
            })
            .collect();
        names.dedup();
        lv.spawns.push(Spawn {
            entity,
            pos: p,
            rot,
            pos_from_path: from_path,
            templates,
            path,
            label: names.join(" / "),
            pos_off,
            rot_off,
            path_idx: path.map(|pe| path_by_entity[&pe]),
            entity_idx,
            alive_off: None,
            alive: true,
        });
    }

    // Rail: course parameters s0 -> scene node -> entity -> path component.
    if let Some(params) = root.table(6) {
        if let Some(node) = params.u32(0) {
            lv.rail = node_entity.get(&node).copied().filter(|e| path_by_entity.contains_key(e));
        }
    }
    lv.species = root.u32s(11);
    if let Some(env) = root.table(13) {
        lv.env_params = env.tables(1).iter().filter_map(|p| p.str(0).map(str::to_string)).collect();
    }
    lv.data = bytes.to_vec();
    lv.orig = bytes.to_vec();
    Ok(lv)
}

impl Level {
    pub fn f32_at(&self, off: usize) -> f32 {
        if off >= VBASE {
            return Buf(&self.extra).f32_at(off - VBASE).unwrap_or(0.0);
        }
        Buf(&self.data).f32_at(off).unwrap_or(0.0)
    }
    pub fn write_f32(&mut self, off: usize, v: f32) {
        let (buf, o) = if off >= VBASE { (&mut self.extra, off - VBASE) } else { (&mut self.data, off) };
        if let Some(b) = buf.get_mut(o..o + 4) {
            b.copy_from_slice(&v.to_le_bytes());
        }
    }
    fn virt(&mut self, floats: usize) -> usize {
        let base = VBASE + self.extra.len();
        self.extra.resize(self.extra.len() + 4 * floats, 0);
        base
    }
    /// Reserves a copy of model instance `src` (not present until its alive flag is written as 1). Returns its index.
    pub fn add_model_slot(&mut self, src: usize) -> usize {
        let base = self.virt(11);
        let mut m = self.models[src].clone();
        m.alive_off = Some(base);
        m.alive = false;
        m.pos_off = Some(base + 4);
        m.rot_off = Some(base + 16);
        m.scale_off = Some(base + 32);
        let (p, r, s) = (m.pos, m.rot, m.scale);
        self.models.push(m);
        for i in 0..3 {
            self.write_f32(base + 4 + 4 * i, p[i]);
            self.write_f32(base + 32 + 4 * i, s[i]);
        }
        for i in 0..4 {
            self.write_f32(base + 16 + 4 * i, r[i]);
        }
        self.models.len() - 1
    }
    /// Reserves a copy of spawn `src` (own-position spawns only). Returns its index.
    pub fn add_spawn_slot(&mut self, src: usize) -> Result<usize, String> {
        let s0 = &self.spawns[src];
        if s0.pos_from_path || s0.pos_off.is_none() || s0.rot_off.is_none() {
            return Err("only Pokémon with their own spawn point can be copied (this one follows a route)".into());
        }
        let mut s = s0.clone();
        let base = self.virt(8);
        s.alive_off = Some(base);
        s.alive = false;
        s.pos_off = Some(base + 4);
        s.rot_off = Some(base + 16);
        let (p, r) = (s.pos, s.rot);
        self.spawns.push(s);
        for i in 0..3 {
            self.write_f32(base + 4 + 4 * i, p[i]);
        }
        for i in 0..4 {
            self.write_f32(base + 16 + 4 * i, r[i]);
        }
        Ok(self.spawns.len() - 1)
    }
    /// The grow requests for the added objects that are present.
    pub fn adds(&self) -> Vec<crate::grow::Add> {
        let mut out = vec![];
        for m in self.models.iter().filter(|m| m.alive_off.is_some() && m.alive) {
            out.push(crate::grow::Add {
                entity: m.entity,
                set: vec![(5, 3, m.pos.to_vec()), (5, 4, m.rot.to_vec()), (5, 5, m.scale.to_vec())],
            });
        }
        for s in self.spawns.iter().filter(|s| s.alive_off.is_some() && s.alive) {
            out.push(crate::grow::Add { entity: s.entity_idx, set: vec![(10, 1, s.pos.to_vec()), (10, 2, s.rot.to_vec())] });
        }
        out
    }
    pub fn has_adds(&self) -> bool {
        self.models.iter().any(|m| m.alive_off.is_some() && m.alive) || self.spawns.iter().any(|s| s.alive_off.is_some() && s.alive)
    }
    /// The bytes to export: the patched file, grown with the added objects.
    pub fn export_bytes(&self) -> Result<Vec<u8>, String> {
        let adds = self.adds();
        if adds.is_empty() {
            return Ok(self.data.clone());
        }
        let (out, _) = crate::grow::grow(&self.data, &adds)?;
        crate::grow::verify(&self.data, &out, &adds)?;
        Ok(out)
    }
    fn read3(&self, off: usize) -> [f32; 3] {
        [self.f32_at(off), self.f32_at(off + 4), self.f32_at(off + 8)]
    }
    /// Re-reads every patchable value from `data` after edits (or undo).
    pub fn refresh(&mut self) {
        for i in 0..self.models.len() {
            let m = &self.models[i];
            let (po, ro, so) = (m.pos_off, m.rot_off, m.scale_off);
            if let Some(o) = po {
                self.models[i].pos = self.read3(o);
            }
            if let Some(o) = ro {
                let r = self.read3(o);
                self.models[i].rot = [r[0], r[1], r[2], self.f32_at(o + 12)];
            }
            if let Some(o) = so {
                self.models[i].scale = self.read3(o);
            }
            if let Some(o) = self.models[i].alive_off {
                self.models[i].alive = self.f32_at(o) != 0.0;
            }
        }
        for pi in 0..self.paths.len() {
            for si in 0..self.paths[pi].offs.len() {
                for k in 0..self.paths[pi].offs[si].len() {
                    let o = self.paths[pi].offs[si][k];
                    self.paths[pi].segments[si][k] = self.read3(o);
                }
            }
        }
        for i in 0..self.spawns.len() {
            let s = &self.spawns[i];
            let (po, ro, from_path, pidx) = (s.pos_off, s.rot_off, s.pos_from_path, s.path_idx);
            if from_path {
                if let Some(p) = pidx.and_then(|p| self.paths[p].start()) {
                    self.spawns[i].pos = p;
                }
            } else if let Some(o) = po {
                self.spawns[i].pos = self.read3(o);
            }
            if let Some(o) = ro {
                let r = self.read3(o);
                self.spawns[i].rot = [r[0], r[1], r[2], self.f32_at(o + 12)];
            }
            if let Some(o) = self.spawns[i].alive_off {
                self.spawns[i].alive = self.f32_at(o) != 0.0;
            }
        }
    }
    pub fn path(&self, entity: u32) -> Option<&PathData> {
        self.paths.iter().find(|p| p.entity == entity)
    }
    pub fn kind(&self) -> &'static str {
        if self.name.starts_with("LV_ENV") {
            "environment"
        } else if self.name.starts_with("LV_MAP") {
            "map (rail, paths, spawns)"
        } else if self.name.starts_with("LV_SND") {
            "sound"
        } else {
            "other"
        }
    }
}
